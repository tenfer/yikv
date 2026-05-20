#pragma once

// KafkaSource — single-partition Kafka consumer embedded in yikv_server.
//
// Consumes JSON messages from one Kafka topic/partition and applies
// INSERT / UPSERT (merge) / DELETE operations to a KVIndex.
//
// Standard JSON message format (single op):
//   { "_op": "INSERT", "_ts": <ms>, "field": value, ... }
//   { "_op": "UPSERT", "_ts": <ms>, "field": value, ... }
//   { "_op": "DELETE", "_ts": <ms>, "<pk_field>": pk_value }
//
// Batch (JSON array in one Kafka message):
//   [ { "_op": "INSERT", ... }, { "_op": "DELETE", ... } ]
//
// Notes:
//  - _op: optional for patch-style incremental updates — omitted or "UPSERT" applies
//         a partial merge (only JSON keys present are written; arrays append).
//         "DELETE" must be explicit. "INSERT" fails if the pk already exists.
//  - _ts: optional (ignored by storage unless a schema field is named "_ts").
//  - Field names match schema field names (not field_id).
//  - Unknown field names are silently ignored.
//  - For DELETE, only the PK field is required.
//  - Single-threaded consumer (CoW single-writer requirement).
//  - Offset is persisted to kafka.offset under the table directory after each message.

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>

#include "index/kv_index.h"
#include "schema/schema.h"

namespace yikv_server::kafka {

class KafkaSource {
public:
    struct Config {
        std::string brokers;
        std::string topic;
        int32_t     partition = 0;
        // Full path: {canonical_table_dir}/kafka.offset
        std::string offset_file;
    };

    KafkaSource(yikv::index::KVIndex*        idx,
                const yikv::schema::Schema*   schema,
                Config                        cfg);
    ~KafkaSource();

    // Start the background consumer thread. Must be called at most once.
    void Start();

    // Signal the consumer to stop and wait for the thread to exit.
    void Stop();

private:
    void ConsumeLoop();

    // Offset persistence.
    int64_t LoadOffset() const;
    void    SaveOffset(int64_t offset) const;

    yikv::index::KVIndex*       idx_;
    const yikv::schema::Schema* schema_;
    Config                      cfg_;

    std::thread       thread_;
    std::atomic<bool> stop_{false};
};

}  // namespace yikv_server::kafka
