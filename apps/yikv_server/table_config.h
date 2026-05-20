#pragma once

// Per-table configuration stored in {db_path}/{table_name}/table.json.
//
// This file is written by the operator (or offline build tool) when deploying
// a table. Only fields relevant to the server runtime are parsed here.
// schema.json (written by the offline build tool) is loaded separately
// via the DB/arena layer.
//
// Minimal example for a real-time table:
//   {
//     "kafka": {
//       "topic":     "my-topic",
//       "partition": 0,
//       "brokers":   "broker1:9092"   // optional; overrides global default
//     }
//   }
//
// For an offline-only table, table.json can be absent or empty.

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

namespace yikv_server {

struct TableKafkaConfig {
    std::string topic;
    int32_t     partition = 0;
    // If non-empty, overrides the global kafka.default_brokers.
    std::string brokers;
};

struct TableConfig {
    std::optional<TableKafkaConfig> kafka;
};

// Returns a default (empty) TableConfig if table.json does not exist.
// Throws std::runtime_error on malformed JSON.
inline TableConfig LoadTableConfig(const std::filesystem::path& table_dir) {
    const auto path = table_dir / "table.json";
    if (!std::filesystem::exists(path)) return {};

    std::ifstream f(path);
    if (!f) return {};

    nlohmann::json j;
    try {
        f >> j;
    } catch (const nlohmann::json::exception& e) {
        throw std::runtime_error("table.json parse error in " + table_dir.string()
                                 + ": " + e.what());
    }

    TableConfig cfg;
    if (j.contains("kafka")) {
        const auto& kj = j["kafka"];
        TableKafkaConfig kc;
        if (!kj.contains("topic") || kj["topic"].get<std::string>().empty())
            throw std::runtime_error("kafka.topic is required in " + path.string());
        kc.topic = kj["topic"].get<std::string>();
        if (kj.contains("partition")) kc.partition = kj["partition"].get<int32_t>();
        if (kj.contains("brokers"))   kc.brokers   = kj["brokers"].get<std::string>();
        cfg.kafka = std::move(kc);
    }
    return cfg;
}

}  // namespace yikv_server
