#include "src/config/server_config.h"

#include "glog/logging.h"
#include "yaml-cpp/yaml.h"

namespace yikv {
namespace config {

namespace {

template <typename T>
void ReadOpt(const YAML::Node& node, const char* key, T& out) {
    if (node[key]) {
        out = node[key].as<T>();
    }
}

}  // namespace

ServerConfig ParseConfig(const std::string& yaml_path) {
    YAML::Node root;
    try {
        root = YAML::LoadFile(yaml_path);
    } catch (const YAML::Exception& e) {
        throw std::runtime_error("Failed to load config file '" + yaml_path +
                                 "': " + e.what());
    }

    if (!root["db_path"] || root["db_path"].as<std::string>().empty()) {
        throw std::runtime_error("Config missing required field 'db_path'");
    }

    ServerConfig cfg;
    cfg.db_path = root["db_path"].as<std::string>();

    ReadOpt(root, "port",                     cfg.port);
    ReadOpt(root, "max_concurrency",          cfg.max_concurrency);
    ReadOpt(root, "idle_timeout_sec",         cfg.idle_timeout_sec);
    ReadOpt(root, "enable_builtin_services",  cfg.enable_builtin_services);

    ReadOpt(root, "cluster",                  cfg.cluster);
    ReadOpt(root, "shard_id",                 cfg.shard_id);
    ReadOpt(root, "total_shards",             cfg.total_shards);
    ReadOpt(root, "host_ip",                  cfg.host_ip);
    ReadOpt(root, "zk_servers",               cfg.zk_servers);
    ReadOpt(root, "zk_root",                  cfg.zk_root);
    ReadOpt(root, "zk_session_timeout_ms",    cfg.zk_session_timeout_ms);

    LOG(INFO) << "Config: db_path=" << cfg.db_path
              << " port=" << cfg.port
              << " cluster=" << cfg.cluster
              << " shard_id=" << cfg.shard_id
              << " total_shards=" << cfg.total_shards
              << " zk_servers=" << (cfg.zk_servers.empty() ? "(disabled)" : cfg.zk_servers);
    return cfg;
}

}  // namespace config
}  // namespace yikv
