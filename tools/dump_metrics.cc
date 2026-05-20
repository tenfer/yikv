#include <iostream>
#include "metrics/metrics.h"

int main() {
    auto& M = yikv_server::metrics::Metrics::instance();
    M.rpc.requests_total.IncFor("get",       "ok");
    M.rpc.requests_total.IncFor("get",       "error");
    M.rpc.requests_total.IncFor("put",       "ok");
    M.rpc.requests_total.IncFor("batch_get", "ok");
    M.rpc.latency_us.RecordFor(123, "get");
    M.rpc.latency_us.RecordFor(456, "put");
    M.kafka.messages_consumed_total.AddFor(42, "users");
    M.kafka.parse_errors_total.IncFor("users");
    M.kafka.committed_offset.SetFor(98765, "users");
    M.kafka.apply_latency_us.RecordFor(900, "users");
    M.tbl.reload_total.IncFor("users",  "ok");
    M.tbl.reload_total.IncFor("orders", "error");
    M.admin.commands_total.IncFor("reload", "ok");
    M.admin.commands_total.IncFor("reload", "error");
    M.set_scrape_callback([](yikv_server::metrics::Metrics& m) {
        m.tbl.table_count.Set(2);
        m.tbl.arena_used_bytes.SetFor(1024 * 1024, "users");
        m.tbl.arena_used_bytes.SetFor(2048, "orders");
        m.tbl.arena_allocation_count.SetFor(101, "users");
        m.tbl.arena_free_count.SetFor(7, "users");
        m.tbl.arena_delayed_count.SetFor(3, "users");
    });
    std::string out;
    M.Render(&out);
    std::cout << out;
    return 0;
}
