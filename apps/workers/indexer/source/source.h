#pragma once

namespace yikv::indexer {

class BoundedParsedBatchQueue;
struct ErrorState;

class Source {
public:
    virtual ~Source() = default;

    /// IO thread entry: claim work and push parsed batches until no more input or error.
    virtual void ProduceLoop(BoundedParsedBatchQueue& q, ErrorState& err) = 0;
};

}  // namespace yikv::indexer
