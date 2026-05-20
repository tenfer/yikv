// yikv-server: multi-table KV index server.
//
// Server JSON: first argument, or if omitted: $YIKV_SERVER_CONFIG, else /etc/yikv/config.json
//   yikv_server
//   yikv_server ./config.json
//
// Per-table config (kafka topic, etc.) lives in {db_path}/{table_name}/table.json
// and is loaded at startup (ScanAndLoad). New directories after start can be
// opened via admin «reload <table_name>» (same as first-time load).
//
// After switching artifact «active» for an existing table, send reload on
// admin_unix_socket (see config) to remap mmap without process restart.

#include "admin_unix_socket.h"
#include "metrics/exposer.h"
#include "metrics/metrics.h"
#include "rpc/db_brpc_service.h"
#include "rpc/db_grpc_service.h"
#include "server_config.h"
#include "table_registry.h"

#include <brpc/server.h>

#include "alloc/allocator.h"
#include "db/db.h"
#include "index/kv_index.h"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

namespace {

constexpr char kDefaultServerConfigPath[] = "/etc/yikv/config.json";
constexpr char kServerConfigEnvVar[]     = "YIKV_SERVER_CONFIG";

const char* ResolveServerConfigPath(int argc, char** argv) {
    if (argc >= 2 && argv[1] != nullptr && argv[1][0] != '\0') {
        return argv[1];
    }
    const char* env = std::getenv(kServerConfigEnvVar);
    if (env != nullptr && env[0] != '\0') {
        return env;
    }
    return kDefaultServerConfigPath;
}

}  // namespace

int main(int argc, char** argv) {
    const char* cfg_path = ResolveServerConfigPath(argc, argv);
    if (argc < 2) {
        std::cerr << "yikv_server: no config path argument; using \"" << cfg_path << "\"\n";
    }

    yikv_server::ServerConfig cfg;
    try {
        cfg = yikv_server::LoadServerConfig(cfg_path);
    } catch (const std::exception& e) {
        std::cerr << "config error: " << e.what() << "\n";
        return 2;
    }

    // ── Init DB ──────────────────────────────────────────────────────────────
    yikv::db::DBOptions opt;
    opt.db_path                       = cfg.db_path;
    opt.alloc_defaults.mode           = yikv::alloc::AllocatorMode::Concurrent;
    const uint64_t seg_b              = cfg.arena_seg_gb * 1024ull * 1024ull * 1024ull;
    opt.alloc_defaults.arena_size     = seg_b;
    opt.alloc_defaults.segment_size   = seg_b;
    opt.alloc_defaults.max_arena_size = cfg.arena_max_gb * 1024ull * 1024ull * 1024ull;
    opt.exclusive_arena_lock          = cfg.exclusive_arena_lock;
    yikv::db::DB::Init(std::move(opt));

    // ── TableRegistry: scan all tables, start KafkaSources ───────────────────
    yikv_server::TableRegistry reg(cfg.db_path, cfg.kafka_default_brokers);

    // Admin socket must come before ScanAndLoad: opening many / large tables can take
    // minutes; deployIndex and start.sh probe connectability before BRPC starts.
    yikv_server::StartAdminUnixSocket(cfg.admin_unix_socket, &reg);

    reg.ScanAndLoad();

    if (reg.TableCount() == 0) {
        std::cerr << "WARNING: no tables found under " << cfg.db_path << "\n";
    }

    // ── RPC services ──────────────────────────────────────────────────────────
    yikv_server::rpc::YikvDbGrpcService grpc_svc(&reg);
    yikv_server::rpc::DbBrpcService*    brpc_svc = new yikv_server::rpc::DbBrpcService(&reg);

    // ── Prometheus /metrics endpoint ─────────────────────────────────────────
    yikv_server::metrics::PromHttpService prom_svc;
    yikv_server::metrics::Metrics::instance().set_scrape_callback(
        [&reg](yikv_server::metrics::Metrics& M) {
            M.tbl.table_count.Set(static_cast<int64_t>(reg.TableCount()));
            reg.ForEach([&](const std::string& name,
                            const yikv_server::TableSlot& slot) {
                if (!slot.kv || !slot.kv->alloc()) return;
                const auto st = slot.kv->alloc()->GetStats();
                M.tbl.arena_used_bytes.SetFor(
                    static_cast<int64_t>(st.used_bytes), name);
                M.tbl.arena_allocation_count.SetFor(
                    static_cast<int64_t>(st.allocation_count), name);
                M.tbl.arena_free_count.SetFor(
                    static_cast<int64_t>(st.free_count), name);
                M.tbl.arena_delayed_count.SetFor(
                    static_cast<int64_t>(st.delayed_count), name);
            });
        });

    brpc::Server        server;
    brpc::ServerOptions sopt;
    sopt.baidu_master_service = brpc_svc;

    if (server.AddService(&grpc_svc, brpc::SERVER_DOESNT_OWN_SERVICE) != 0) {
        std::cerr << "Fail to add yikv.db.YikvDb gRPC service\n";
        return 1;
    }
    if (server.AddService(&prom_svc, brpc::SERVER_DOESNT_OWN_SERVICE,
                          "/metrics => Scrape") != 0) {
        std::cerr << "Fail to add Prometheus /metrics service\n";
        return 1;
    }
    if (server.Start(cfg.listen.c_str(), &sopt) != 0) {
        std::cerr << "Fail to start server on " << cfg.listen << "\n";
        return 1;
    }
    std::cerr << "yikv-server listening on " << cfg.listen
              << " (baidu_std + h2:grpc, /metrics enabled)\n";

    server.RunUntilAskedToQuit();

    yikv_server::metrics::Metrics::instance().set_scrape_callback({});
    return 0;
}
