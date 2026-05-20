#include "indexer/queue/parsed_batch_queue.h"
#include "indexer/source/sql/hive_source.h"

#include <arrow/api.h>

namespace yikv::indexer {

void HiveSource::ProduceLoop(BoundedParsedBatchQueue& q, ErrorState& err) {
    err.Set(arrow::Status::NotImplemented("HiveSource"));
    q.Abort();
}

}  // namespace yikv::indexer
