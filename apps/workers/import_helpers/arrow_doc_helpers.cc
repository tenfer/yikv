#include "import_helpers/arrow_doc_helpers.h"

#include <climits>
#include <string_view>
#include <vector>

namespace yikv_import {

using yikv::index::Doc;
using yikv::schema::DataType;
using yikv::schema::FieldDef;

bool ArrowCellIsNull(const arrow::Array& col, int64_t row) { return col.IsNull(row); }

static arrow::Status ApplyScalar(Doc* doc, const FieldDef& def, const arrow::Array& col, int64_t row) {
    if (ArrowCellIsNull(col, row)) return arrow::Status::OK();
    const uint32_t fid = def.field_id;
    switch (def.type) {
        case DataType::Bool: {
            if (col.type_id() != arrow::Type::BOOL) {
                return arrow::Status::Invalid("field ", def.name, ": expected bool column");
            }
            const auto& a = static_cast<const arrow::BooleanArray&>(col);
            doc->put_int32(fid, a.Value(row) ? 1 : 0);
            return arrow::Status::OK();
        }
        case DataType::Int32: {
            int32_t v = 0;
            switch (col.type_id()) {
                case arrow::Type::INT8:
                    v = static_cast<const arrow::Int8Array&>(col).Value(row);
                    break;
                case arrow::Type::INT16:
                    v = static_cast<const arrow::Int16Array&>(col).Value(row);
                    break;
                case arrow::Type::INT32:
                    v = static_cast<const arrow::Int32Array&>(col).Value(row);
                    break;
                case arrow::Type::UINT8:
                    v = static_cast<const arrow::UInt8Array&>(col).Value(row);
                    break;
                case arrow::Type::UINT16:
                    v = static_cast<const arrow::UInt16Array&>(col).Value(row);
                    break;
                case arrow::Type::UINT32: {
                    uint32_t u = static_cast<const arrow::UInt32Array&>(col).Value(row);
                    if (u > static_cast<uint32_t>(INT32_MAX))
                        return arrow::Status::Invalid("field ", def.name, ": uint32 too large for int32");
                    v = static_cast<int32_t>(u);
                    break;
                }
                case arrow::Type::INT64: {
                    int64_t x = static_cast<const arrow::Int64Array&>(col).Value(row);
                    if (x > INT32_MAX || x < INT32_MIN)
                        return arrow::Status::Invalid("field ", def.name, ": int64 out of int32 range");
                    v = static_cast<int32_t>(x);
                    break;
                }
                case arrow::Type::UINT64: {
                    uint64_t u = static_cast<const arrow::UInt64Array&>(col).Value(row);
                    if (u > static_cast<uint64_t>(INT32_MAX))
                        return arrow::Status::Invalid("field ", def.name, ": uint64 too large for int32");
                    v = static_cast<int32_t>(u);
                    break;
                }
                default:
                    return arrow::Status::Invalid("field ", def.name, ": unsupported Arrow type for int32");
            }
            doc->put_int32(fid, v);
            return arrow::Status::OK();
        }
        case DataType::Int64: {
            int64_t v = 0;
            switch (col.type_id()) {
                case arrow::Type::INT64:
                    v = static_cast<const arrow::Int64Array&>(col).Value(row);
                    break;
                case arrow::Type::INT32:
                    v = static_cast<const arrow::Int32Array&>(col).Value(row);
                    break;
                case arrow::Type::UINT32:
                    v = static_cast<int64_t>(static_cast<const arrow::UInt32Array&>(col).Value(row));
                    break;
                case arrow::Type::UINT64: {
                    uint64_t u = static_cast<const arrow::UInt64Array&>(col).Value(row);
                    if (u > static_cast<uint64_t>(INT64_MAX))
                        return arrow::Status::Invalid("field ", def.name, ": uint64 too large for int64");
                    v = static_cast<int64_t>(u);
                    break;
                }
                default:
                    return arrow::Status::Invalid("field ", def.name, ": unsupported type for int64");
            }
            doc->put_int64(fid, v);
            return arrow::Status::OK();
        }
        case DataType::Float32: {
            if (col.type_id() != arrow::Type::FLOAT) {
                return arrow::Status::Invalid("field ", def.name, ": expected float");
            }
            const auto& a = static_cast<const arrow::FloatArray&>(col);
            doc->put_float(fid, a.Value(row));
            return arrow::Status::OK();
        }
        case DataType::Float64: {
            if (col.type_id() != arrow::Type::DOUBLE) {
                return arrow::Status::Invalid("field ", def.name, ": expected double");
            }
            const auto& a = static_cast<const arrow::DoubleArray&>(col);
            doc->put_double(fid, a.Value(row));
            return arrow::Status::OK();
        }
        case DataType::String:
        case DataType::Bytes: {
            if (col.type_id() == arrow::Type::STRING) {
                const auto& a = static_cast<const arrow::StringArray&>(col);
                auto v = a.GetView(row);
                doc->put_string(fid, v);
            } else if (col.type_id() == arrow::Type::BINARY) {
                const auto& a = static_cast<const arrow::BinaryArray&>(col);
                auto v = a.GetView(row);
                doc->put_string(fid, std::string_view(v.data(), static_cast<size_t>(v.size())));
            } else {
                return arrow::Status::Invalid("field ", def.name, ": expected string/binary");
            }
            return arrow::Status::OK();
        }
        default:
            return arrow::Status::Invalid("unsupported schema DataType for field ", def.name);
    }
}

template<typename ListArrayType>
static arrow::Status ApplyListFixed(Doc* doc, const FieldDef& def, const ListArrayType& list_arr, int64_t row,
                                    DataType elem_dt) {
    if (list_arr.IsNull(row)) return arrow::Status::OK();
    const int64_t off = list_arr.value_offset(row);
    const int64_t len = list_arr.value_length(row);
    if (len == 0) {
        switch (elem_dt) {
            case DataType::Int32:    doc->array_put_int32(def.field_id, nullptr, 0); break;
            case DataType::Int64:    doc->array_put_int64(def.field_id, nullptr, 0); break;
            case DataType::Float32:  doc->array_put_float(def.field_id, nullptr, 0); break;
            case DataType::Float64:  doc->array_put_double(def.field_id, nullptr, 0); break;
            case DataType::String:   doc->array_put_string(def.field_id, nullptr, 0); break;
            default: break;
        }
        return arrow::Status::OK();
    }
    auto values = std::static_pointer_cast<arrow::Array>(list_arr.values());
    switch (elem_dt) {
        case DataType::Int32: {
            const auto* a = static_cast<const arrow::Int32Array*>(values.get());
            doc->array_put_int32(def.field_id, a->raw_values() + off, static_cast<uint32_t>(len));
            return arrow::Status::OK();
        }
        case DataType::Int64: {
            const auto* a = static_cast<const arrow::Int64Array*>(values.get());
            doc->array_put_int64(def.field_id, a->raw_values() + off, static_cast<uint32_t>(len));
            return arrow::Status::OK();
        }
        case DataType::Float32: {
            const auto* a = static_cast<const arrow::FloatArray*>(values.get());
            doc->array_put_float(def.field_id, a->raw_values() + off, static_cast<uint32_t>(len));
            return arrow::Status::OK();
        }
        case DataType::Float64: {
            const auto* a = static_cast<const arrow::DoubleArray*>(values.get());
            doc->array_put_double(def.field_id, a->raw_values() + off, static_cast<uint32_t>(len));
            return arrow::Status::OK();
        }
        case DataType::String: {
            if (values->type_id() != arrow::Type::STRING) {
                return arrow::Status::Invalid("field ", def.name, ": list<string> expected STRING values");
            }
            const auto* a = static_cast<const arrow::StringArray*>(values.get());
            std::vector<std::string_view> parts;
            parts.reserve(static_cast<size_t>(len));
            for (int64_t i = 0; i < len; ++i) {
                auto v = a->GetView(off + i);
                parts.emplace_back(v.data(), static_cast<size_t>(v.size()));
            }
            doc->array_put_string(def.field_id, parts.data(), static_cast<uint32_t>(parts.size()));
            return arrow::Status::OK();
        }
        default:
            return arrow::Status::Invalid("unsupported list element type");
    }
}

arrow::Status ApplyField(Doc* doc, const FieldDef& def, const std::shared_ptr<arrow::Array>& col, int64_t row) {
    if (!def.is_array) return ApplyScalar(doc, def, *col, row);

    switch (col->type_id()) {
        case arrow::Type::LIST:
            return ApplyListFixed(doc, def, static_cast<const arrow::ListArray&>(*col), row, def.type);
        case arrow::Type::LARGE_LIST:
            return ApplyListFixed(doc, def, static_cast<const arrow::LargeListArray&>(*col), row, def.type);
        default:
            return arrow::Status::Invalid("field ", def.name, ": expected list column for is_array");
    }
}

}  // namespace yikv_import
