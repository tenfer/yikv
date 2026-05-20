#include "metrics/metrics.h"

#include <unordered_set>

namespace yikv_server::metrics {

namespace {

constexpr const char* kBuildVersion =
#ifdef YIKV_BUILD_VERSION
    YIKV_BUILD_VERSION
#else
    "dev"
#endif
    ;

// Render a single metric family with proper HELP/TYPE header (emitted once
// per family name across all label combinations).
struct FamilyRenderer {
    std::string*               out;
    std::unordered_set<std::string> emitted_headers;

    void operator()(const MetricSample& s) {
        if (emitted_headers.insert(s.name).second) {
            RenderHelpType(s, out);
        }
        RenderSampleLines(s, out);
    }
};

}  // namespace

Metrics& Metrics::instance() {
    static Metrics m;
    return m;
}

Metrics::Metrics() {
    proc.build_info.SetFor(1, kBuildVersion);
}

void Metrics::set_scrape_callback(ScrapeCallback cb) {
    std::lock_guard lk(cb_mu_);
    scrape_cb_ = std::move(cb);
}

void Metrics::on_scrape() {
    ScrapeCallback cb;
    {
        std::lock_guard lk(cb_mu_);
        cb = scrape_cb_;
    }
    if (cb) cb(*this);
}

void Metrics::Render(std::string* out) {
    on_scrape();

    FamilyRenderer fam{out, {}};

    // ── RPC ──
    rpc.requests_total.Visit(std::ref(fam));
    rpc.latency_us.Visit(std::ref(fam));

    // ── Kafka ──
    kafka.messages_consumed_total.Visit(std::ref(fam));
    kafka.parse_errors_total.Visit(std::ref(fam));
    kafka.apply_errors_total.Visit(std::ref(fam));
    kafka.committed_offset.Visit(std::ref(fam));
    kafka.apply_latency_us.Visit(std::ref(fam));

    // ── Table / Allocator ──
    tbl.table_count.Visit(std::ref(fam));
    tbl.reload_total.Visit(std::ref(fam));
    tbl.arena_used_bytes.Visit(std::ref(fam));
    tbl.arena_allocation_count.Visit(std::ref(fam));
    tbl.arena_free_count.Visit(std::ref(fam));
    tbl.arena_delayed_count.Visit(std::ref(fam));

    // ── Admin ──
    admin.commands_total.Visit(std::ref(fam));

    // ── Process ──
    proc.build_info.Visit(std::ref(fam));
}

}  // namespace yikv_server::metrics
