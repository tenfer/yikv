#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>

namespace yikv {
namespace config {

struct ServerConfig {
    // ── 存储 ──────────────────────────────────────────────────────────────────
    std::string db_path;             // 根目录；每个子目录对应一张表

    // ── 网络 ──────────────────────────────────────────────────────────────────
    int  port                    = 8000;
    int  max_concurrency         = 0;    // 0 = unlimited
    int  idle_timeout_sec        = 30;
    bool enable_builtin_services = true;

    // ── 分布式：分片 & 副本 ───────────────────────────────────────────────────
    std::string cluster      = "default"; // 集群名，对应 ZK 路径中的一级节点
    int         shard_id     = 0;         // 本实例负责的分片编号 [0, total_shards)
    int         total_shards = 1;         // 集群总分片数
    std::string host_ip;                  // 注册到 ZK 的 IP（空则自动探测）

    // ── ZooKeeper 服务注册 ────────────────────────────────────────────────────
    std::string zk_servers;                // "host1:2181,host2:2181"；空则禁用 ZK
    std::string zk_root           = "/yikv";
    int         zk_session_timeout_ms = 5000;
};

// 解析 YAML 配置文件，失败时抛出 std::runtime_error
ServerConfig ParseConfig(const std::string& yaml_path);

}  // namespace config
}  // namespace yikv
