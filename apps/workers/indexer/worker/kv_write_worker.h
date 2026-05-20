#pragma once

#include "indexer/queue/parsed_batch_queue.h"

#include "index/kv_index.h"
#include "schema/schema.h"

#include <arrow/api.h>

#include <chrono>
#include <cstdint>

namespace yikv::indexer {

struct ImportProgress {
    std::chrono::steady_clock::time_point t_start{};
    std::chrono::steady_clock::time_point t_last_log{};
    std::uint64_t                         rows_at_last_log = 0;
    std::uint64_t                         next_milestone   = 10'000ULL;
};

arrow::Status ProcessBatch(yikv::index::KVIndex* idx, const yikv::schema::Schema& sch,
                           const ParsedBatch& pb, std::uint64_t* rows_imported,
                           std::uint64_t* skipped_pk_null, ImportProgress* prog);

/// Pop until EOS: single writer thread; sets `err` and Aborts queue on failure.
void RunKvWriteLoop(yikv::index::KVIndex* idx, const yikv::schema::Schema* sch,
                    BoundedParsedBatchQueue& queue, ErrorState& err, std::uint64_t* total_rows,
                    std::uint64_t* skipped_pk_null, ImportProgress* prog);

}  // namespace yikv::indexer
