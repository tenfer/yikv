#pragma once

// Metrics facade for yikv-server.
//
// Singleton entry-point: Metrics::instance(). All business code reaches
// metrics through one of the domain sub-structs (rpc / kafka / tbl / admin /
// proc). Each metric is declared ONCE here, with stable name + HELP + label
// schema, so this header is the canonical list of what the server exports.
//
// Design notes:
//   - Counters/Gauges/Summaries are owned by this singleton; no factory layer.
//   - RPC handlers use RpcScope (RAII) — destructor stamps the appropriate
//     counter (by method × status) and records request latency in one call.
//   - Allocator/table_count gauges are pull-style: filled by on_scrape() right
//     before the exposer renders. Wiring is done via set_scrape_callback() in
//     main.cc to avoid a metrics_lib ↔ table_registry_lib build cycle.
//   - This header avoids brpc.h to keep build-time poison surface small;
//     LatencyRecorder is only pulled in through labeled.h.

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "metrics/labeled.h"

namespace yikv_server::metrics {

class Metrics {
public:
    static Metrics& instance();

    using ScrapeCallback = std::function<void(Metrics&)>;

    // Register a callback invoked at scrape time to refill pull-style gauges
    // (allocator stats, table count). Pass {} to clear. main.cc wires this to
    // a TableRegistry-aware lambda so metrics_lib has no compile-time
    // dependency on TableRegistry.
    void set_scrape_callback(ScrapeCallback cb);

    // Iterate every metric and emit Prometheus exposition text into `out`.
    // Acquires read locks on each labeled metric; safe to call concurrently
    // with business inc/set/record calls.
    void Render(std::string* out);

    // ─── RPC domain ───────────────────────────────────────────────────────────
    struct Rpc {
        // labels: method ∈ {get,put,put_batch,batch_get}; status ∈ {ok,error}.
        LabeledCounter<2> requests_total{
            "yikv_rpc_requests_total",
            "Total number of RPC requests handled, by method and result status.",
            {"method", "status"}};
        LabeledLatency<1> latency_us{
            "yikv_rpc_latency_us",
            "RPC request latency (microseconds), by method. Reported as Summary.",
            {"method"}};

        // RAII scope: stamps counter+latency on destruction.
        class Scope {
        public:
            Scope(Rpc* owner, std::string_view method)
                : owner_(owner), method_(method), t0_(Clock::now()) {}

            ~Scope() {
                if (!owner_) return;
                const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                                    Clock::now() - t0_).count();
                owner_->requests_total.IncFor(method_, status_);
                owner_->latency_us.RecordFor(static_cast<int64_t>(us), method_);
            }

            Scope(const Scope&)            = delete;
            Scope& operator=(const Scope&) = delete;
            Scope(Scope&& o) noexcept
                : owner_(o.owner_),
                  method_(std::move(o.method_)),
                  status_(std::move(o.status_)),
                  t0_(o.t0_) {
                o.owner_ = nullptr;
            }

            void mark_status(std::string_view s) { status_ = s; }
            void mark_error()                     { status_ = "error"; }

        private:
            using Clock = std::chrono::steady_clock;
            Rpc*                              owner_;
            std::string                       method_;
            std::string                       status_{"ok"};
            std::chrono::time_point<Clock>    t0_;
        };

        Scope scope(std::string_view method) { return Scope(this, method); }
    } rpc;

    // ─── Kafka domain ─────────────────────────────────────────────────────────
    struct Kafka {
        LabeledCounter<1> messages_consumed_total{
            "yikv_kafka_messages_consumed_total",
            "Number of Kafka messages successfully consumed and applied, by table.",
            {"table"}};
        LabeledCounter<1> parse_errors_total{
            "yikv_kafka_parse_errors_total",
            "Number of Kafka messages that failed JSON parsing, by table.",
            {"table"}};
        LabeledCounter<1> apply_errors_total{
            "yikv_kafka_apply_errors_total",
            "Number of Kafka messages whose row could not be applied to the KV index, by table.",
            {"table"}};
        LabeledGauge<1> committed_offset{
            "yikv_kafka_committed_offset",
            "Most recently persisted offset, by table.",
            {"table"}};
        LabeledLatency<1> apply_latency_us{
            "yikv_kafka_apply_latency_us",
            "Time spent applying a Kafka message (parse + index write), microseconds.",
            {"table"}};
    } kafka;

    // ─── Table / Allocator domain ─────────────────────────────────────────────
    struct Tbl {
        SingleGauge       table_count{
            "yikv_table_count",
            "Number of loaded tables in this server."};
        LabeledCounter<2> reload_total{
            "yikv_table_reload_total",
            "Number of ReloadTable / first-load completions, by table and result.",
            {"table", "result"}};

        // Allocator snapshot gauges (filled at scrape time by on_scrape()).
        LabeledGauge<1>   arena_used_bytes{
            "yikv_arena_used_bytes",
            "Bytes currently used in the arena, by table.",
            {"table"}};
        LabeledGauge<1>   arena_allocation_count{
            "yikv_arena_allocation_count",
            "Cumulative arena allocation count, by table.",
            {"table"}};
        LabeledGauge<1>   arena_free_count{
            "yikv_arena_free_count",
            "Cumulative arena Free() count, by table.",
            {"table"}};
        LabeledGauge<1>   arena_delayed_count{
            "yikv_arena_delayed_count",
            "Cumulative arena delayed-free block count, by table.",
            {"table"}};
    } tbl;

    // ─── Admin domain ─────────────────────────────────────────────────────────
    struct Admin {
        LabeledCounter<2> commands_total{
            "yikv_admin_commands_total",
            "Number of admin commands processed, by command name and result.",
            {"cmd", "result"}};
    } admin;

    // ─── Process domain ───────────────────────────────────────────────────────
    struct Proc {
        LabeledGauge<1>   build_info{
            "yikv_build_info",
            "Build / version info as label dimensions; value is always 1.",
            {"version"}};
    } proc;

    // Invoke the scrape callback (if any). Called automatically by Render();
    // also exposed so unit tests can verify behaviour directly.
    void on_scrape();

private:
    Metrics();
    ~Metrics()                         = default;
    Metrics(const Metrics&)            = delete;
    Metrics& operator=(const Metrics&) = delete;

    std::mutex      cb_mu_;
    ScrapeCallback  scrape_cb_;
};

}  // namespace yikv_server::metrics
