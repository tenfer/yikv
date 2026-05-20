#pragma once

#include "indexer/queue/parsed_batch_queue.h"
#include "indexer/source/source.h"

#include "schema/schema.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace yikv::indexer {

/// Connection + query settings for MySQL-compatible servers (MySQL, MariaDB, StarRocks FE, …).
struct MysqlWireConfig {
    std::string             host;
    std::uint16_t           port        = 3306;
    std::string              user;
    std::string              password;
    std::string              database;
    /// Main SELECT (or read-only statement returning a result set).
    std::string              query;
    /// Optional session setup (SET NAMES, warehouse/session vars, …).
    std::vector<std::string> init_sql;
    /// Rows per RecordBatch pushed to the queue (default chosen for moderate memory).
    std::size_t batch_rows = 4096;
};

/// Streams a MySQL wire-protocol result set into Arrow RecordBatches matching the yikv schema column names.
///
/// Only one `ProduceLoop` instance runs the query: `IoPublisher` may start several threads, but non-leaders exit
/// immediately (same pattern as single-stream sources). One MYSQL connection must not be shared across threads.
class MysqlWireSource : public Source {
public:
    MysqlWireSource(MysqlWireConfig cfg, const yikv::schema::Schema* schema);

    void ProduceLoop(BoundedParsedBatchQueue& q, ErrorState& err) override;

private:
    MysqlWireConfig              cfg_;
    const yikv::schema::Schema* schema_{nullptr};
    std::atomic<bool>            consumed_{false};
};

}  // namespace yikv::indexer
