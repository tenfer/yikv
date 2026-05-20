#pragma once

#include "indexer/queue/parsed_batch_queue.h"
#include "indexer/source/file/cloud_filesystem.h"
#include "indexer/source/source.h"

#include "schema/schema.h"

#include <atomic>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace yikv::indexer {

/// Local paths and cloud URIs (oss/s3/cos/obs/gs): Parquet + CSV, multi-IO-thread file claiming.
class FileSource : public Source {
public:
    FileSource(std::vector<std::string> paths, const yikv::schema::Schema* schema, CloudFileSystems cloud = {});

    void ProduceLoop(BoundedParsedBatchQueue& q, ErrorState& err) override;

private:
    std::vector<std::string>    paths_;
    const yikv::schema::Schema* schema_{nullptr};
    CloudFileSystems            cloud_;
    std::atomic<std::size_t>    next_file_{0};
};

}  // namespace yikv::indexer
