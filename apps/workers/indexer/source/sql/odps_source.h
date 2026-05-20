#pragma once

#include "indexer/source/source.h"

namespace yikv::indexer {

class OdpsSource : public Source {
public:
    void ProduceLoop(BoundedParsedBatchQueue& q, ErrorState& err) override;
};

}  // namespace yikv::indexer
