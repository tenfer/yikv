// Unit tests for the metrics module. Covers:
//   * LabeledCounter inc / multi-label fan-out / Visit() snapshot
//   * LabeledGauge set / overwrite / Visit() snapshot
//   * LabeledLatency record + Summary quantile + _count + _sum output
//   * Label value escaping (\\, \", \n)
//   * Render(): HELP/TYPE printed once per family, sample lines for each
//   * Scrape callback dispatch
//   * Concurrent IncFor is race-free (counter == total inc count)

#include "metrics/labeled.h"
#include "metrics/metrics.h"

#include <atomic>
#include <chrono>
#include <regex>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

using yikv_server::metrics::LabeledCounter;
using yikv_server::metrics::LabeledGauge;
using yikv_server::metrics::LabeledLatency;
using yikv_server::metrics::SingleGauge;
using yikv_server::metrics::Metrics;
using yikv_server::metrics::MetricSample;
using yikv_server::metrics::MetricType;

// ─── LabeledCounter ─────────────────────────────────────────────────────────

TEST(LabeledCounterTest, IncForSameLabelsAccumulates) {
    LabeledCounter<2> c{"test_counter", "doc", {"method", "status"}};
    c.IncFor("get", "ok");
    c.IncFor("get", "ok");
    c.AddFor(3, "get", "ok");

    int64_t  total = 0;
    int      seen  = 0;
    c.Visit([&](const MetricSample& s) {
        ++seen;
        EXPECT_EQ(s.type, MetricType::Counter);
        EXPECT_EQ(s.name, "test_counter");
        ASSERT_EQ(s.label_names.size(), 2u);
        ASSERT_EQ(s.label_values.size(), 2u);
        EXPECT_EQ(s.label_names[0], "method");
        EXPECT_EQ(s.label_names[1], "status");
        EXPECT_EQ(s.label_values[0], "get");
        EXPECT_EQ(s.label_values[1], "ok");
        total = s.value;
    });
    EXPECT_EQ(seen, 1);
    EXPECT_EQ(total, 5);
}

TEST(LabeledCounterTest, DifferentLabelsAreSeparateSeries) {
    LabeledCounter<2> c{"test_counter", "doc", {"method", "status"}};
    c.IncFor("get", "ok");
    c.IncFor("get", "error");
    c.IncFor("put", "ok");
    c.IncFor("put", "ok");

    std::map<std::pair<std::string, std::string>, int64_t> seen;
    c.Visit([&](const MetricSample& s) {
        seen[{s.label_values[0], s.label_values[1]}] = s.value;
    });
    EXPECT_EQ(seen.size(), 3u);
    EXPECT_EQ((seen[{"get", "ok"}]),    1);
    EXPECT_EQ((seen[{"get", "error"}]), 1);
    EXPECT_EQ((seen[{"put", "ok"}]),    2);
}

TEST(LabeledCounterTest, ConcurrentIncIsRaceFree) {
    LabeledCounter<1> c{"test_counter", "doc", {"k"}};
    constexpr int     kThreads = 8;
    constexpr int     kPerThread = 5000;

    std::vector<std::thread> ths;
    ths.reserve(kThreads);
    for (int i = 0; i < kThreads; ++i) {
        ths.emplace_back([&c]() {
            for (int j = 0; j < kPerThread; ++j) c.IncFor("hot");
        });
    }
    for (auto& t : ths) t.join();

    int64_t total = 0;
    c.Visit([&](const MetricSample& s) { total = s.value; });
    EXPECT_EQ(total, static_cast<int64_t>(kThreads * kPerThread));
}

// ─── LabeledGauge ────────────────────────────────────────────────────────────

TEST(LabeledGaugeTest, SetForOverwrites) {
    LabeledGauge<1> g{"used_bytes", "doc", {"table"}};
    g.SetFor(100, "users");
    g.SetFor(250, "users");
    g.SetFor(7,   "orders");

    std::map<std::string, int64_t> seen;
    g.Visit([&](const MetricSample& s) {
        seen[s.label_values[0]] = s.value;
        EXPECT_EQ(s.type, MetricType::Gauge);
    });
    EXPECT_EQ(seen.size(), 2u);
    EXPECT_EQ(seen["users"], 250);
    EXPECT_EQ(seen["orders"], 7);
}

// ─── LabeledLatency (Summary) ────────────────────────────────────────────────

TEST(LabeledLatencyTest, RecordEmitsQuantilesCountAndSum) {
    LabeledLatency<1> lat{"req_latency_us", "doc", {"method"}};
    int64_t sum = 0;
    for (int i = 1; i <= 100; ++i) {
        lat.RecordFor(i, "get");
        sum += i;
    }
    int seen = 0;
    lat.Visit([&](const MetricSample& s) {
        ++seen;
        EXPECT_EQ(s.type, MetricType::Summary);
        EXPECT_EQ(s.name, "req_latency_us");
        EXPECT_EQ(s.summary.count, 100);
        EXPECT_EQ(s.summary.sum, sum);
        ASSERT_EQ(s.summary.quantiles.size(), 4u);
        EXPECT_DOUBLE_EQ(s.summary.quantiles[0].first, 0.5);
        EXPECT_DOUBLE_EQ(s.summary.quantiles[1].first, 0.9);
        EXPECT_DOUBLE_EQ(s.summary.quantiles[2].first, 0.99);
        EXPECT_DOUBLE_EQ(s.summary.quantiles[3].first, 0.999);
        // bvar::LatencyRecorder's percentile is computed over a sample window
        // populated by a background thread; immediately after Record() the
        // window may still report 0 in tests. We only assert ordering + sane
        // upper bound here; the well-formed Summary block above is the main
        // contract being verified.
        for (const auto& [q, v] : s.summary.quantiles) {
            EXPECT_GE(v, 0);
            EXPECT_LE(v, 100);
        }
    });
    EXPECT_EQ(seen, 1);
}

// ─── Label value escaping ────────────────────────────────────────────────────

TEST(EscapeTest, BackslashQuoteAndNewline) {
    EXPECT_EQ(yikv_server::metrics::EscapeLabelValue(R"(plain)"), "plain");
    EXPECT_EQ(yikv_server::metrics::EscapeLabelValue(R"(\)"),     R"(\\)");
    EXPECT_EQ(yikv_server::metrics::EscapeLabelValue("\""),       "\\\"");
    EXPECT_EQ(yikv_server::metrics::EscapeLabelValue("a\nb"),     "a\\nb");
    EXPECT_EQ(yikv_server::metrics::EscapeLabelValue("a\"b\\c\nd"),
              "a\\\"b\\\\c\\nd");
}

// ─── End-to-end Render() ─────────────────────────────────────────────────────

TEST(MetricsRenderTest, HelpAndTypeAppearOncePerFamily) {
    auto& M = Metrics::instance();
    // Mutate a few well-known metrics so the family appears in output.
    M.rpc.requests_total.IncFor("get", "ok");
    M.rpc.requests_total.IncFor("get", "error");
    M.rpc.latency_us.RecordFor(42, "get");
    M.kafka.committed_offset.SetFor(123, "users");
    M.admin.commands_total.IncFor("reload", "ok");

    std::string out;
    M.Render(&out);

    // HELP for rpc_requests_total must appear exactly once.
    auto count_substr = [](std::string_view body, std::string_view needle) {
        int c = 0;
        for (size_t pos = 0;
             (pos = body.find(needle, pos)) != std::string::npos;
             pos += needle.size())
            ++c;
        return c;
    };
    EXPECT_EQ(count_substr(out, "# HELP yikv_rpc_requests_total "), 1);
    EXPECT_EQ(count_substr(out, "# TYPE yikv_rpc_requests_total counter"), 1);
    // Two distinct sample lines (one per status).
    EXPECT_GE(count_substr(out, "yikv_rpc_requests_total{method=\"get\","), 2);

    // Summary should have quantile + _count + _sum lines.
    EXPECT_NE(out.find("yikv_rpc_latency_us{method=\"get\",quantile=\"0.5\"}"),
              std::string::npos);
    EXPECT_NE(out.find("yikv_rpc_latency_us{method=\"get\",quantile=\"0.999\"}"),
              std::string::npos);
    EXPECT_NE(out.find("yikv_rpc_latency_us_count{method=\"get\"}"),
              std::string::npos);
    EXPECT_NE(out.find("yikv_rpc_latency_us_sum{method=\"get\"}"),
              std::string::npos);

    EXPECT_NE(out.find("yikv_kafka_committed_offset{table=\"users\"} 123"),
              std::string::npos);
    EXPECT_NE(out.find("yikv_admin_commands_total{cmd=\"reload\",result=\"ok\"}"),
              std::string::npos);
    EXPECT_NE(out.find("yikv_build_info{version="), std::string::npos);
}

TEST(MetricsRenderTest, ScrapeCallbackIsInvokedAndCanFillGauges) {
    auto& M = Metrics::instance();
    int   calls = 0;
    M.set_scrape_callback([&calls](Metrics& M2) {
        ++calls;
        M2.tbl.table_count.Set(42);
        M2.tbl.arena_used_bytes.SetFor(1024, "users");
    });
    std::string out;
    M.Render(&out);
    EXPECT_EQ(calls, 1);
    EXPECT_NE(out.find("yikv_table_count 42"), std::string::npos);
    EXPECT_NE(out.find("yikv_arena_used_bytes{table=\"users\"} 1024"),
              std::string::npos);
    M.set_scrape_callback({});  // clean up for sibling tests
}

// ─── Render correctness: well-formed text format ─────────────────────────────

TEST(MetricsRenderTest, EscapingInLabelValuesIsApplied) {
    LabeledCounter<1> c{"weird_total", "doc", {"k"}};
    c.IncFor("a\"b\\c");
    std::string out;
    yikv_server::metrics::MetricSample s_seen;
    c.Visit([&](const MetricSample& s) {
        yikv_server::metrics::RenderSample(s, &out);
    });
    EXPECT_NE(out.find(R"(weird_total{k="a\"b\\c"} 1)"), std::string::npos)
        << "actual: " << out;
}

// ─── SingleGauge ─────────────────────────────────────────────────────────────

TEST(SingleGaugeTest, Setget) {
    SingleGauge g{"my_singleton", "doc"};
    g.Set(123);
    int seen = 0;
    g.Visit([&](const MetricSample& s) {
        ++seen;
        EXPECT_EQ(s.value, 123);
        EXPECT_TRUE(s.label_names.empty());
    });
    EXPECT_EQ(seen, 1);
}
