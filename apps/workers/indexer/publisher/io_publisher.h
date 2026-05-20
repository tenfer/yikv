#pragma once

namespace yikv::indexer {

class Source;
class BoundedParsedBatchQueue;
struct ErrorState;

class IoPublisher {
public:
    /// Spawns `num_workers` threads; each calls `src.ProduceLoop(q, err)` (shared source, e.g. atomic file index).
    static void Run(Source& src, unsigned num_workers, BoundedParsedBatchQueue& q, ErrorState& err);
};

}  // namespace yikv::indexer
