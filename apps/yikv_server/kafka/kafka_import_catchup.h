#pragma once

// One-shot Kafka consume after offline full import: seek by timestamp (with rewind),
// apply the same merge rules as KafkaSource, then write {table_dir}/kafka.offset and
// kafka_meta.json.

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>

namespace yikv::index {
class KVIndex;
}
namespace yikv::schema {
class Schema;
}

namespace yikv_server::kafka {

struct KafkaImportCatchupOptions {
    std::string brokers;
    std::string topic;
    int32_t     partition              = 0;
    int64_t     offline_watermark_sec  = 0;
    uint32_t    rewind_minutes         = 0;
    int         offsets_query_timeout_ms = 30'000;
    int         consume_timeout_ms       = 200;
    int         max_silence_loops        = 3;
    // After EOF, poll this many empty cycles before exiting (handles in-flight produce).
    int         max_wall_seconds         = 7200;
    std::function<void(std::string_view)> log_info;
    std::function<void(std::string_view)> log_err;
};

// Returns false on transport / config failure. Writes offset + meta on success even if
// no messages were consumed (starts at high watermark).
bool RunKafkaImportCatchup(const std::filesystem::path& table_dir,
                           yikv::index::KVIndex*        idx,
                           const yikv::schema::Schema*  schema,
                           const KafkaImportCatchupOptions& opt);

}  // namespace yikv_server::kafka
