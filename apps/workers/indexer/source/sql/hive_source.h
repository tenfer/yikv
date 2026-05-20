#pragma once

#include "indexer/source/source.h"

namespace yikv::indexer {

class HiveSource : public Source {
public:
    void ProduceLoop(BoundedParsedBatchQueue& q, ErrorState& err) override;
};

}  // namespace yikv::indexer
