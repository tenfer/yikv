#pragma once

// Server-level configuration loaded from config.json.
// Invocation: yikv_server [config.json] — omit to use $YIKV_SERVER_CONFIG or /etc/yikv/config.json
//
// Optional JSON key "artifact_storage" (shape: ../../config.example.json)
// is ignored by the server but read by Python tooling (pipeline_config / artifact_sync -c).
//
// Table-specific configuration (kafka topic, etc.) lives in
// {db_path}/{table_name}/table.json and is loaded dynamically.

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>

namespace yikv_server {

struct ServerConfig {
    // Required: directory that contains per-table sub-directories.
    std::string db_path;

    // Network.
    std::string listen = "0.0.0.0:9000";

    // Arena sizing (applied to all tables sharing this DB).
    uint64_t arena_seg_gb = 1;
    uint64_t arena_max_gb = 512;

    // When false, DB does not flock(arena.lock) before mmap (dangerous if two writers).
    bool exclusive_arena_lock = true;

    // Global default Kafka brokers; individual table.json can override.
    std::string kafka_default_brokers;

    // Optional AF_UNIX path: send line `reload <table>` to reopen index after symlink swap.
    // When omitted or empty, defaults to {parent of db_path}/admin.sock (same as pipeline WORK/admin.sock
    // when db_path is WORK/server_db).
    std::string admin_unix_socket;
};

inline ServerConfig LoadServerConfig(const std::string& path) {
    namespace fs = std::filesystem;
    std::ifstream f(path);
    if (!f) throw std::runtime_error("cannot open config file: " + path);
    nlohmann::json j;
    try {
        f >> j;
    } catch (const nlohmann::json::exception& e) {
        throw std::runtime_error("config JSON parse error: " + std::string(e.what()));
    }

    ServerConfig cfg;

    if (!j.contains("db_path") || j["db_path"].get<std::string>().empty())
        throw std::runtime_error("config missing required key: db_path");
    cfg.db_path = j["db_path"].get<std::string>();

    if (j.contains("listen"))       cfg.listen       = j["listen"].get<std::string>();
    if (j.contains("arena_seg_gb")) cfg.arena_seg_gb = j["arena_seg_gb"].get<uint64_t>();
    if (j.contains("arena_max_gb")) cfg.arena_max_gb = j["arena_max_gb"].get<uint64_t>();

    if (j.contains("exclusive_arena_lock"))
        cfg.exclusive_arena_lock = j["exclusive_arena_lock"].get<bool>();

    if (j.contains("kafka") && j["kafka"].contains("default_brokers"))
        cfg.kafka_default_brokers = j["kafka"]["default_brokers"].get<std::string>();

    if (j.contains("admin_unix_socket"))
        cfg.admin_unix_socket = j["admin_unix_socket"].get<std::string>();

    // Trim whitespace; treat all-blank as unset → default next to db tree root.
    {
        auto& s = cfg.admin_unix_socket;
        const auto not_space = [](unsigned char c) { return !std::isspace(c); };
        s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
        s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
    }
    if (cfg.admin_unix_socket.empty()) {
        fs::path dbp(cfg.db_path);
        fs::path parent = dbp.has_parent_path() ? dbp.parent_path() : fs::path(".");
        cfg.admin_unix_socket = (parent / "admin.sock").lexically_normal().string();
        std::cerr << "config: admin_unix_socket not set; using default " << cfg.admin_unix_socket << "\n";
    }

    if (cfg.arena_seg_gb == 0 || cfg.arena_max_gb == 0)
        throw std::runtime_error("arena_seg_gb and arena_max_gb must be positive");
    if (cfg.arena_max_gb < cfg.arena_seg_gb)
        throw std::runtime_error("arena_max_gb must be >= arena_seg_gb");

    return cfg;
}

}  // namespace yikv_server
