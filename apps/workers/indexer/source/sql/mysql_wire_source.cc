#include "indexer/source/sql/mysql_wire_source.h"

#include <arrow/api.h>
#include <arrow/builder.h>

#include <climits>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <string_view>

#include <strings.h>

#include <mysql/mysql.h>

namespace yikv::indexer {
namespace {

using yikv::schema::DataType;
using yikv::schema::FieldDef;
using yikv::schema::Schema;

static bool NameEqualInsensitive(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        const char ca = static_cast<char>(std::tolower(static_cast<unsigned char>(a[i])));
        const char cb = static_cast<char>(std::tolower(static_cast<unsigned char>(b[i])));
        if (ca != cb) return false;
    }
    return true;
}

static arrow::Status ValidateNoArrayFields(const Schema& sch) {
    for (const auto& fp : sch.fields()) {
        if (fp.get()->is_array) {
            return arrow::Status::Invalid("MysqlWireSource: array fields not supported (", fp.get()->name,
                                          "); CAST in SQL or use file import");
        }
    }
    return arrow::Status::OK();
}

static arrow::Result<std::shared_ptr<arrow::Schema>> ArrowSchemaFromYikv(const Schema& sch) {
    arrow::FieldVector fields;
    fields.reserve(sch.fields().size());
    for (const auto& fp : sch.fields()) {
        const FieldDef* d = fp.get();
        std::shared_ptr<arrow::DataType> t;
        switch (d->type) {
            case DataType::Bool: t = arrow::boolean(); break;
            case DataType::Int32: t = arrow::int32(); break;
            case DataType::Int64: t = arrow::int64(); break;
            case DataType::Float32: t = arrow::float32(); break;
            case DataType::Float64: t = arrow::float64(); break;
            case DataType::String:
            case DataType::Bytes: t = arrow::utf8(); break;
            default:
                return arrow::Status::Invalid("MysqlWireSource: unsupported yikv field type for ", d->name);
        }
        fields.push_back(arrow::field(d->name, std::move(t), true));
    }
    return arrow::schema(std::move(fields));
}

/// Map each yikv field index -> MySQL result column index (by column name, case-insensitive).
static arrow::Status MapColumns(const Schema& sch, MYSQL_RES* result, std::vector<int>* col_index) {
    const unsigned nmysql  = mysql_num_fields(result);
    MYSQL_FIELD*   mfields = mysql_fetch_fields(result);
    const int      n       = static_cast<int>(sch.fields().size());
    col_index->assign(static_cast<size_t>(n), -1);

    for (int i = 0; i < n; ++i) {
        const FieldDef* d = sch.fields()[static_cast<size_t>(i)].get();
        int             found = -1;
        for (unsigned j = 0; j < nmysql; ++j) {
            const std::string_view colname(mfields[j].name, mfields[j].name_length);
            if (!NameEqualInsensitive(d->name, colname)) continue;
            if (found >= 0) {
                return arrow::Status::Invalid("MysqlWireSource: ambiguous duplicate name for column ", d->name);
            }
            found = static_cast<int>(j);
        }
        if (found < 0) {
            return arrow::Status::Invalid("MysqlWireSource: result set missing column: ", d->name);
        }
        (*col_index)[static_cast<size_t>(i)] = found;
    }
    return arrow::Status::OK();
}

static arrow::Status AppendBool(arrow::BooleanBuilder* b, const char* data, size_t len, bool is_null) {
    if (is_null) return b->AppendNull();
    if (len == 1 && (data[0] == '0' || data[0] == '1')) return b->Append(data[0] == '1');
    if (len == 4 && strncasecmp(data, "true", 4) == 0) return b->Append(true);
    if (len == 5 && strncasecmp(data, "false", 5) == 0) return b->Append(false);
    char*    end = nullptr;
    long     v   = std::strtol(data, &end, 10);
    const auto consumed = static_cast<size_t>(end - data);
    if (end == data || consumed != len) {
        return arrow::Status::Invalid("MysqlWireSource: bool parse failed");
    }
    return b->Append(v != 0);
}

static arrow::Status AppendInt32(arrow::Int32Builder* b, const char* data, size_t len, bool is_null) {
    if (is_null) return b->AppendNull();
    if (len == 0) return arrow::Status::Invalid("MysqlWireSource: empty int32");
    char* end = nullptr;
    long  v   = std::strtol(data, &end, 10);
    if (end != data + len) return arrow::Status::Invalid("MysqlWireSource: int32 parse failed");
    if (v > INT32_MAX || v < INT32_MIN) return arrow::Status::Invalid("MysqlWireSource: int32 out of range");
    return b->Append(static_cast<int32_t>(v));
}

static arrow::Status AppendInt64(arrow::Int64Builder* b, const char* data, size_t len, bool is_null) {
    if (is_null) return b->AppendNull();
    if (len == 0) return arrow::Status::Invalid("MysqlWireSource: empty int64");
    char*      end = nullptr;
    long long v   = std::strtoll(data, &end, 10);
    if (end != data + len) return arrow::Status::Invalid("MysqlWireSource: int64 parse failed");
    return b->Append(static_cast<int64_t>(v));
}

static arrow::Status AppendFloat(arrow::FloatBuilder* b, const char* data, size_t len, bool is_null) {
    if (is_null) return b->AppendNull();
    if (len == 0) return arrow::Status::Invalid("MysqlWireSource: empty float");
    char* end = nullptr;
    auto  v   = std::strtof(data, &end);
    if (end != data + len) return arrow::Status::Invalid("MysqlWireSource: float parse failed");
    return b->Append(v);
}

static arrow::Status AppendDouble(arrow::DoubleBuilder* b, const char* data, size_t len, bool is_null) {
    if (is_null) return b->AppendNull();
    if (len == 0) return arrow::Status::Invalid("MysqlWireSource: empty double");
    char* end = nullptr;
    auto  v   = std::strtod(data, &end);
    if (end != data + len) return arrow::Status::Invalid("MysqlWireSource: double parse failed");
    return b->Append(v);
}

static arrow::Status AppendUtf8(arrow::StringBuilder* b, const char* data, size_t len, bool is_null) {
    if (is_null) return b->AppendNull();
    return b->Append(data, static_cast<int32_t>(len));
}

static arrow::Status AppendForField(const FieldDef& def, arrow::ArrayBuilder* base, const char* data, size_t len,
                                    bool is_null) {
    switch (def.type) {
        case DataType::Bool:
            return AppendBool(static_cast<arrow::BooleanBuilder*>(base), data, len, is_null);
        case DataType::Int32:
            return AppendInt32(static_cast<arrow::Int32Builder*>(base), data, len, is_null);
        case DataType::Int64:
            return AppendInt64(static_cast<arrow::Int64Builder*>(base), data, len, is_null);
        case DataType::Float32:
            return AppendFloat(static_cast<arrow::FloatBuilder*>(base), data, len, is_null);
        case DataType::Float64:
            return AppendDouble(static_cast<arrow::DoubleBuilder*>(base), data, len, is_null);
        case DataType::String:
        case DataType::Bytes:
            return AppendUtf8(static_cast<arrow::StringBuilder*>(base), data, len, is_null);
        default: return arrow::Status::Invalid("MysqlWireSource: internal type");
    }
}

struct MysqlConn {
    MYSQL* mysql = nullptr;
    MysqlConn()  = default;
    ~MysqlConn() {
        if (mysql) mysql_close(mysql);
    }
    MysqlConn(const MysqlConn&)            = delete;
    MysqlConn& operator=(const MysqlConn&) = delete;
};

static arrow::Status RunQuery(MYSQL* mysql, const std::string& sql) {
    if (mysql_real_query(mysql, sql.data(), sql.size()) != 0) {
        return arrow::Status::Invalid("MysqlWireSource SQL error: ", mysql_error(mysql));
    }
    MYSQL_RES* one = mysql_store_result(mysql);
    if (one) mysql_free_result(one);
    if (mysql_field_count(mysql) != 0) {
        return arrow::Status::Invalid("MysqlWireSource: init SQL returned a result set (use non-SELECT or consume)");
    }
    return arrow::Status::OK();
}

static arrow::Status FlushBatch(const Schema& sch, const std::shared_ptr<arrow::Schema>& aschema,
                                std::vector<std::unique_ptr<arrow::ArrayBuilder>>& builders,
                                BoundedParsedBatchQueue& q, ErrorState& err) {
    std::vector<std::shared_ptr<arrow::Array>> arrays;
    arrays.reserve(builders.size());
    int64_t nrows = -1;
    for (size_t i = 0; i < builders.size(); ++i) {
        std::shared_ptr<arrow::Array> arr;
        auto st = builders[i]->Finish(&arr);
        if (!st.ok()) {
            err.Set(st);
            q.Abort();
            return st;
        }
        if (nrows < 0) nrows = arr->length();
        else if (arr->length() != nrows) {
            auto ist = arrow::Status::Invalid("MysqlWireSource: ragged builders");
            err.Set(ist);
            q.Abort();
            return ist;
        }
        arrays.push_back(std::move(arr));
    }
    if (nrows <= 0) return arrow::Status::OK();
    auto batch = arrow::RecordBatch::Make(aschema, nrows, std::move(arrays));
    if (!batch) {
        auto ist = arrow::Status::Invalid("MysqlWireSource: RecordBatch::Make failed");
        err.Set(ist);
        q.Abort();
        return ist;
    }
    ParsedBatch pb;
    pb.batch             = std::move(batch);
    pb.path              = "mysql:";
    pb.file_index_1based = 0;
    q.Push(std::move(pb));

    // Re-create builders for next batch
    builders.clear();
    for (const auto& fp : sch.fields()) {
        const FieldDef* d = fp.get();
        switch (d->type) {
            case DataType::Bool: builders.push_back(std::make_unique<arrow::BooleanBuilder>()); break;
            case DataType::Int32: builders.push_back(std::make_unique<arrow::Int32Builder>()); break;
            case DataType::Int64: builders.push_back(std::make_unique<arrow::Int64Builder>()); break;
            case DataType::Float32: builders.push_back(std::make_unique<arrow::FloatBuilder>()); break;
            case DataType::Float64: builders.push_back(std::make_unique<arrow::DoubleBuilder>()); break;
            case DataType::String:
            case DataType::Bytes: builders.push_back(std::make_unique<arrow::StringBuilder>()); break;
            default: return arrow::Status::Invalid("internal");
        }
    }
    return arrow::Status::OK();
}

}  // namespace

MysqlWireSource::MysqlWireSource(MysqlWireConfig cfg, const Schema* schema)
    : cfg_(std::move(cfg)), schema_(schema) {}

void MysqlWireSource::ProduceLoop(BoundedParsedBatchQueue& q, ErrorState& err) {
    bool expected = false;
    if (!consumed_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) return;

    if (!schema_) {
        err.Set(arrow::Status::Invalid("MysqlWireSource: null schema"));
        q.Abort();
        return;
    }
    err.Set(ValidateNoArrayFields(*schema_));
    if (err.failed.load()) {
        q.Abort();
        return;
    }

    MysqlConn conn_holder;
    conn_holder.mysql = mysql_init(nullptr);
    if (!conn_holder.mysql) {
        err.Set(arrow::Status::OutOfMemory("mysql_init failed"));
        q.Abort();
        return;
    }
    MYSQL* mysql = conn_holder.mysql;
    if (mysql_options(mysql, MYSQL_SET_CHARSET_NAME, "utf8mb4") != 0) {
        err.Set(arrow::Status::Invalid("MysqlWireSource: mysql_options MYSQL_SET_CHARSET_NAME"));
        q.Abort();
        return;
    }
    // Large reads (StarRocks / analytic): keep default net buffer unless tune needed.
    if (!mysql_real_connect(mysql, cfg_.host.c_str(), cfg_.user.c_str(), cfg_.password.c_str(),
                            cfg_.database.c_str(), cfg_.port,
                            /*unix_socket=*/nullptr, /*clientflag=*/0)) {
        err.Set(arrow::Status::Invalid("MysqlWireSource connect failed: ", mysql_error(mysql)));
        q.Abort();
        return;
    }

    for (const auto& stmt : cfg_.init_sql) {
        if (stmt.empty()) continue;
        err.Set(RunQuery(mysql, stmt));
        if (err.failed.load()) {
            q.Abort();
            return;
        }
    }

    if (mysql_real_query(mysql, cfg_.query.data(), cfg_.query.size()) != 0) {
        err.Set(arrow::Status::Invalid("MysqlWireSource query error: ", mysql_error(mysql)));
        q.Abort();
        return;
    }

    MYSQL_RES* result = mysql_use_result(mysql);
    if (!result) {
        if (mysql_field_count(mysql) != 0) {
            err.Set(arrow::Status::Invalid("MysqlWireSource: mysql_use_result failed: ", mysql_error(mysql)));
            q.Abort();
            return;
        }
        // No result set (e.g. DDL-only) — nothing to import.
        return;
    }

    struct ResFree {
        MYSQL_RES* r;
        ~ResFree() {
            if (r) mysql_free_result(r);
        }
    } res_guard{result};

    std::vector<int> col_index;
    err.Set(MapColumns(*schema_, result, &col_index));
    if (err.failed.load()) {
        q.Abort();
        return;
    }

    arrow::Result<std::shared_ptr<arrow::Schema>> asc_res = ArrowSchemaFromYikv(*schema_);
    if (!asc_res.ok()) {
        err.Set(asc_res.status());
        q.Abort();
        return;
    }
    std::shared_ptr<arrow::Schema> aschema = std::move(asc_res).ValueOrDie();

    std::vector<std::unique_ptr<arrow::ArrayBuilder>> builders;
    for (const auto& fp : schema_->fields()) {
        const FieldDef* d = fp.get();
        switch (d->type) {
            case DataType::Bool: builders.push_back(std::make_unique<arrow::BooleanBuilder>()); break;
            case DataType::Int32: builders.push_back(std::make_unique<arrow::Int32Builder>()); break;
            case DataType::Int64: builders.push_back(std::make_unique<arrow::Int64Builder>()); break;
            case DataType::Float32: builders.push_back(std::make_unique<arrow::FloatBuilder>()); break;
            case DataType::Float64: builders.push_back(std::make_unique<arrow::DoubleBuilder>()); break;
            case DataType::String:
            case DataType::Bytes: builders.push_back(std::make_unique<arrow::StringBuilder>()); break;
            default: {
                err.Set(arrow::Status::Invalid("internal builder"));
                q.Abort();
                return;
            }
        }
    }

    const size_t    nf  = schema_->fields().size();
    const std::size_t batch_sz = cfg_.batch_rows < 1 ? 4096 : cfg_.batch_rows;
    std::size_t     in_batch = 0;

    while (!err.failed.load()) {
        MYSQL_ROW row = mysql_fetch_row(result);
        if (!row) {
            if (mysql_errno(mysql) != 0) {
                err.Set(arrow::Status::Invalid("MysqlWireSource fetch: ", mysql_error(mysql)));
                q.Abort();
            }
            break;
        }
        unsigned long* lens = mysql_fetch_lengths(result);

        for (size_t fi = 0; fi < nf; ++fi) {
            const FieldDef* def = schema_->fields()[fi].get();
            const int       j   = col_index[fi];
            const bool      is_null = (row[j] == nullptr);
            const size_t    len       = is_null ? 0 : static_cast<size_t>(lens[j]);
            const char*     data      = row[j];
            err.Set(AppendForField(*def, builders[fi].get(), data, len, is_null));
            if (err.failed.load()) {
                q.Abort();
                return;
            }
        }
        ++in_batch;
        if (in_batch >= batch_sz) {
            err.Set(FlushBatch(*schema_, aschema, builders, q, err));
            if (err.failed.load()) return;
            in_batch = 0;
        }
    }

    if (in_batch > 0) {
        err.Set(FlushBatch(*schema_, aschema, builders, q, err));
    }
}

}  // namespace yikv::indexer
