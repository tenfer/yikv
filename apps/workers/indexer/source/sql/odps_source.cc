#include "indexer/queue/parsed_batch_queue.h"
#include "indexer/source/sql/odps_source.h"

#include <arrow/api.h>

namespace yikv::indexer {

void OdpsSource::ProduceLoop(BoundedParsedBatchQueue& q, ErrorState& err) {
    err.Set(arrow::Status::NotImplemented("OdpsSource"));
    q.Abort();
}

}  // namespace yikv::indexer
