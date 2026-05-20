#include "indexer/worker/kv_write_worker.h"

#include "import_helpers/arrow_doc_helpers.h"

#include "index/doc.h"

#include <algorithm>
#include <ctime>
#include <iostream>
#include <string>
#include <vector>

namespace yikv::indexer {
namespace {

std::string WallTimestamp() {
    const auto        now = std::chrono::system_clock::now();
    const std::time_t t   = std::chrono::system_clock::to_time_t(now);
    struct tm         tm_buf {};
    ::localtime_r(&t, &tm_buf);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm_buf);
    return buf;
}

static constexpr std::uint64_t kProgressLogEveryRows = 10'000ULL;
// Bound peak FtAllocator usage per Arrow RecordBatch. Large Parquet row groups can be
// millions of rows; processing them in one shot allocates all Docs in the arena before
// BatchPut runs and can hit arena_max_gb with std::bad_alloc even on hosts with lots of RAM.
static constexpr int64_t kImportChunkRows = 16 * 1024;

static arrow::Status ValidateBatchColumns(const arrow::RecordBatch& batch, const yikv::schema::Schema& sch) {
    for (const auto& fp : sch.fields()) {
        const yikv::schema::FieldDef* def = fp.get();
        if (!batch.GetColumnByName(def->name)) {
            return arrow::Status::Invalid("parquet missing column: ", def->name);
        }
    }
    return arrow::Status::OK();
}

static void LogImportMilestone(std::uint64_t total_imported, ImportProgress* prog) {
    const auto       now          = std::chrono::steady_clock::now();
    const double     interval_sec = std::chrono::duration<double>(now - prog->t_last_log).count();
    const std::uint64_t d_rows    = total_imported - prog->rows_at_last_log;
    const double     interval_tps = interval_sec > 0 ? static_cast<double>(d_rows) / interval_sec : 0.0;
    const double     overall_sec  = std::chrono::duration<double>(now - prog->t_start).count();
    const double     overall_tps  = overall_sec > 0 ? static_cast<double>(total_imported) / overall_sec : 0.0;

    std::cerr << WallTimestamp() << " import_pipeline progress total_rows=" << total_imported
              << " interval_tps=" << static_cast<long long>(interval_tps + 0.5)
              << " overall_tps=" << static_cast<long long>(overall_tps + 0.5) << "\n";

    prog->t_last_log       = now;
    prog->rows_at_last_log = total_imported;
}

static void MaybeLogImportProgress(std::uint64_t total_imported, ImportProgress* prog) {
    if (total_imported < prog->next_milestone) return;
    LogImportMilestone(total_imported, prog);
    prog->next_milestone = (total_imported / kProgressLogEveryRows + 1) * kProgressLogEveryRows;
}

}  // namespace

arrow::Status ProcessBatch(yikv::index::KVIndex* idx, const yikv::schema::Schema& sch,
                           const ParsedBatch& pb, std::uint64_t* rows_imported,
                           std::uint64_t* skipped_pk_null, ImportProgress* prog) {
    const auto&       batch = *pb.batch;
    ARROW_RETURN_NOT_OK(ValidateBatchColumns(batch, sch));
    const int64_t                n      = batch.num_rows();
    const yikv::schema::FieldDef* pk_def = sch.FindField(sch.pk());
    if (!pk_def) return arrow::Status::Invalid("schema has no pk");

    for (int64_t chunk_start = 0; chunk_start < n; chunk_start += kImportChunkRows) {
        const int64_t chunk_end = std::min(n, chunk_start + kImportChunkRows);
        std::vector<yikv::index::Doc>  batch_docs;
        std::vector<yikv::index::Doc*> batch_ptrs;
        const size_t                   chunk_cap = static_cast<size_t>(chunk_end - chunk_start);
        batch_docs.reserve(chunk_cap);
        batch_ptrs.reserve(chunk_cap);

        for (int64_t r = chunk_start; r < chunk_end; ++r) {
            auto pk_col = batch.GetColumnByName(pk_def->name);
            if (!pk_col || yikv_import::ArrowCellIsNull(*pk_col, r)) {
                ++(*skipped_pk_null);
                continue;
            }

            yikv::index::Doc doc = idx->NewDoc();
            for (const auto& fp : sch.fields()) {
                const yikv::schema::FieldDef* def = fp.get();
                auto                           col = batch.GetColumnByName(def->name);
                ARROW_RETURN_NOT_OK(yikv_import::ApplyField(&doc, *def, col, r));
            }

            std::string pk_str;
            switch (pk_def->type) {
                case yikv::schema::DataType::Bool:
                    pk_str = std::to_string(doc.get_int32(pk_def->field_id));
                    break;
                case yikv::schema::DataType::Int32:
                    pk_str = std::to_string(doc.get_int32(pk_def->field_id));
                    break;
                case yikv::schema::DataType::Int64:
                    pk_str = std::to_string(doc.get_int64(pk_def->field_id));
                    break;
                case yikv::schema::DataType::String:
                case yikv::schema::DataType::Bytes: {
                    auto sv = doc.get_string(pk_def->field_id);
                    pk_str.assign(sv.data(), sv.size());
                    break;
                }
                default: return arrow::Status::Invalid("unsupported pk type for import");
            }
            if (pk_str.empty()) {
                ++(*skipped_pk_null);
                continue;
            }

            batch_docs.push_back(std::move(doc));
            batch_ptrs.push_back(&batch_docs.back());
        }

        if (!batch_ptrs.empty()) {
            idx->BatchPut(batch_ptrs);
            *rows_imported += batch_ptrs.size();
            MaybeLogImportProgress(*rows_imported, prog);
        }
    }
    (void)pb.file_index_1based;
    (void)pb.path;
    return arrow::Status::OK();
}

void RunKvWriteLoop(yikv::index::KVIndex* idx, const yikv::schema::Schema* sch,
                    BoundedParsedBatchQueue& queue, ErrorState& err, std::uint64_t* total_rows,
                    std::uint64_t* skipped_pk_null, ImportProgress* prog) {
    if (!sch) {
        err.Set(arrow::Status::Invalid("RunKvWriteLoop: null schema"));
        queue.Abort();
        return;
    }
    while (true) {
        std::optional<ParsedBatch> item = queue.Pop();
        if (!item.has_value()) break;
        arrow::Status ws = ProcessBatch(idx, *sch, *item, total_rows, skipped_pk_null, prog);
        if (!ws.ok()) {
            err.Set(ws);
            queue.Abort();
            break;
        }
    }
}

}  // namespace yikv::indexer
