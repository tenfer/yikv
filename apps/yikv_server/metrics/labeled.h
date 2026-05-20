#pragma once

// Labeled bvar wrappers (small custom layer on top of brpc::bvar).
//
// brpc::bvar has no native concept of Prometheus-style labels. We implement
// labels at the C++ level: each LabeledCounter<N>/LabeledGauge<N>/
// LabeledLatency<N> holds an internal map keyed by `array<string,N>` of label
// values; child bvar objects are created lazily on first observation.
//
// Output is produced via Visit(): the exposer iterates every (name, labels,
// type, value-or-quantile-set) entry and renders Prometheus exposition text.
//
// Concurrency model:
//   - bvar::Adder / bvar::Status<int64_t> / bvar::LatencyRecorder are
//     internally thread-safe (lock-free or RW-locked per-CPU).
//   - The only thing we lock is the unordered_map lookup/insert under
//     std::mutex `mu_`. Hot path takes a read into the map (shared_lock is
//     overkill for small maps); cold path (first observation per label
//     tuple) does an emplace.

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <bvar/bvar.h>
#include <bvar/latency_recorder.h>
#include <bvar/reducer.h>
#include <bvar/status.h>

namespace yikv_server::metrics {

enum class MetricType { Counter, Gauge, Summary };

// Snapshot of one labeled value (or one summary entry) seen by an exposer.
struct MetricSample {
    MetricType                          type;
    std::string                         name;       // Prometheus metric name
    std::string                         help;
    std::vector<std::string>            label_names;
    std::vector<std::string>            label_values;
    // For Counter/Gauge:
    int64_t                             value = 0;
    // For Summary (LatencyRecorder): quantile points + _count + _sum suffixes
    // are emitted by the caller using these fields:
    struct SummaryView {
        int64_t count = 0;
        int64_t sum   = 0;
        // (quantile, value) pairs
        std::vector<std::pair<double, int64_t>> quantiles;
    } summary;
};

using SampleVisitor = std::function<void(const MetricSample&)>;

// ─── Hashing helpers ──────────────────────────────────────────────────────────

template <std::size_t N>
struct LabelKey {
    std::array<std::string, N> values;
    bool operator==(const LabelKey& o) const noexcept { return values == o.values; }
};

template <std::size_t N>
struct LabelKeyHash {
    std::size_t operator()(const LabelKey<N>& k) const noexcept {
        // FNV-1a 64-bit over the joined label values, with NUL separators.
        std::uint64_t h = 1469598103934665603ull;
        for (std::size_t i = 0; i < N; ++i) {
            for (unsigned char c : k.values[i]) {
                h ^= c;
                h *= 1099511628211ull;
            }
            h ^= 0;
            h *= 1099511628211ull;
        }
        return static_cast<std::size_t>(h);
    }
};

// ─── LabeledCounter ───────────────────────────────────────────────────────────

template <std::size_t N>
class LabeledCounter {
public:
    using Names = std::array<std::string, N>;

    LabeledCounter(std::string name, std::string help, Names label_names)
        : name_(std::move(name)), help_(std::move(help)), label_names_(std::move(label_names)) {}

    template <typename... Values>
    void IncFor(Values&&... vs) {
        static_assert(sizeof...(Values) == N, "label count mismatch");
        LabelKey<N> k{{ToString(std::forward<Values>(vs))...}};
        GetOrCreate(k).fetch_add(1, std::memory_order_relaxed);
    }
    template <typename... Values>
    void AddFor(int64_t delta, Values&&... vs) {
        static_assert(sizeof...(Values) == N, "label count mismatch");
        LabelKey<N> k{{ToString(std::forward<Values>(vs))...}};
        GetOrCreate(k).fetch_add(delta, std::memory_order_relaxed);
    }

    // Visit each (label_values, current_value) pair.
    void Visit(const SampleVisitor& fn) const {
        std::lock_guard lk(mu_);
        for (const auto& [k, entry] : map_) {
            MetricSample s;
            s.type        = MetricType::Counter;
            s.name        = name_;
            s.help        = help_;
            s.label_names = LabelNamesVec();
            s.label_values.assign(k.values.begin(), k.values.end());
            s.value       = entry->load(std::memory_order_relaxed);
            fn(s);
        }
    }

    const std::string& name() const noexcept { return name_; }

private:
    std::atomic<int64_t>& GetOrCreate(const LabelKey<N>& k) {
        {
            std::lock_guard lk(mu_);
            auto it = map_.find(k);
            if (it != map_.end()) return *it->second;
            auto cell = std::make_unique<std::atomic<int64_t>>(0);
            auto& ref = *cell;
            map_.emplace(k, std::move(cell));
            return ref;
        }
    }

    std::vector<std::string> LabelNamesVec() const {
        return std::vector<std::string>(label_names_.begin(), label_names_.end());
    }

    template <typename T>
    static std::string ToString(T&& v) {
        if constexpr (std::is_convertible_v<T, std::string>)
            return std::string(std::forward<T>(v));
        else if constexpr (std::is_convertible_v<T, std::string_view>)
            return std::string(std::string_view(std::forward<T>(v)));
        else
            return std::to_string(std::forward<T>(v));
    }

    std::string  name_;
    std::string  help_;
    Names        label_names_;
    mutable std::mutex mu_;
    std::unordered_map<LabelKey<N>, std::unique_ptr<std::atomic<int64_t>>,
                       LabelKeyHash<N>>
        map_;
};

// ─── LabeledGauge ─────────────────────────────────────────────────────────────

template <std::size_t N>
class LabeledGauge {
public:
    using Names = std::array<std::string, N>;

    LabeledGauge(std::string name, std::string help, Names label_names)
        : name_(std::move(name)), help_(std::move(help)), label_names_(std::move(label_names)) {}

    template <typename... Values>
    void SetFor(int64_t v, Values&&... vs) {
        static_assert(sizeof...(Values) == N, "label count mismatch");
        LabelKey<N> k{{ToString(std::forward<Values>(vs))...}};
        GetOrCreate(k).store(v, std::memory_order_relaxed);
    }

    void Visit(const SampleVisitor& fn) const {
        std::lock_guard lk(mu_);
        for (const auto& [k, entry] : map_) {
            MetricSample s;
            s.type        = MetricType::Gauge;
            s.name        = name_;
            s.help        = help_;
            s.label_names.assign(label_names_.begin(), label_names_.end());
            s.label_values.assign(k.values.begin(), k.values.end());
            s.value       = entry->load(std::memory_order_relaxed);
            fn(s);
        }
    }

    const std::string& name() const noexcept { return name_; }

private:
    std::atomic<int64_t>& GetOrCreate(const LabelKey<N>& k) {
        std::lock_guard lk(mu_);
        auto it = map_.find(k);
        if (it != map_.end()) return *it->second;
        auto cell = std::make_unique<std::atomic<int64_t>>(0);
        auto& ref = *cell;
        map_.emplace(k, std::move(cell));
        return ref;
    }

    template <typename T>
    static std::string ToString(T&& v) {
        if constexpr (std::is_convertible_v<T, std::string>)
            return std::string(std::forward<T>(v));
        else if constexpr (std::is_convertible_v<T, std::string_view>)
            return std::string(std::string_view(std::forward<T>(v)));
        else
            return std::to_string(std::forward<T>(v));
    }

    std::string  name_;
    std::string  help_;
    Names        label_names_;
    mutable std::mutex mu_;
    std::unordered_map<LabelKey<N>, std::unique_ptr<std::atomic<int64_t>>,
                       LabelKeyHash<N>>
        map_;
};

// ─── LabeledLatency ──────────────────────────────────────────────────────────
//
// Backed by bvar::LatencyRecorder (for quantiles + max) plus a parallel
// bvar::Adder<int64_t> "sum" so we can emit a stable _sum counter that
// reflects ALL recorded observations (LatencyRecorder's window-windowed
// average is not suitable for Prometheus Summary's _sum semantics).

template <std::size_t N>
class LabeledLatency {
public:
    using Names = std::array<std::string, N>;

    LabeledLatency(std::string name, std::string help, Names label_names)
        : name_(std::move(name)), help_(std::move(help)), label_names_(std::move(label_names)) {}

    template <typename... Values>
    void RecordFor(int64_t us, Values&&... vs) {
        static_assert(sizeof...(Values) == N, "label count mismatch");
        LabelKey<N> k{{ToString(std::forward<Values>(vs))...}};
        auto& e = GetOrCreate(k);
        e.lr << us;
        e.sum << us;
    }

    void Visit(const SampleVisitor& fn) const {
        std::lock_guard lk(mu_);
        for (const auto& [k, entry] : map_) {
            MetricSample s;
            s.type        = MetricType::Summary;
            s.name        = name_;
            s.help        = help_;
            s.label_names.assign(label_names_.begin(), label_names_.end());
            s.label_values.assign(k.values.begin(), k.values.end());
            s.summary.count = entry->lr.count();
            s.summary.sum   = entry->sum.get_value();
            s.summary.quantiles.reserve(4);
            s.summary.quantiles.emplace_back(0.5,   entry->lr.latency_percentile(0.5));
            s.summary.quantiles.emplace_back(0.9,   entry->lr.latency_percentile(0.9));
            s.summary.quantiles.emplace_back(0.99,  entry->lr.latency_percentile(0.99));
            s.summary.quantiles.emplace_back(0.999, entry->lr.latency_percentile(0.999));
            fn(s);
        }
    }

    const std::string& name() const noexcept { return name_; }

private:
    struct Entry {
        ::bvar::LatencyRecorder    lr;
        ::bvar::Adder<int64_t>     sum;
    };

    Entry& GetOrCreate(const LabelKey<N>& k) {
        std::lock_guard lk(mu_);
        auto it = map_.find(k);
        if (it != map_.end()) return *it->second;
        auto entry = std::make_unique<Entry>();
        auto& ref = *entry;
        map_.emplace(k, std::move(entry));
        return ref;
    }

    template <typename T>
    static std::string ToString(T&& v) {
        if constexpr (std::is_convertible_v<T, std::string>)
            return std::string(std::forward<T>(v));
        else if constexpr (std::is_convertible_v<T, std::string_view>)
            return std::string(std::string_view(std::forward<T>(v)));
        else
            return std::to_string(std::forward<T>(v));
    }

    std::string  name_;
    std::string  help_;
    Names        label_names_;
    mutable std::mutex mu_;
    std::unordered_map<LabelKey<N>, std::unique_ptr<Entry>, LabelKeyHash<N>> map_;
};

// ─── Single-value gauge (no labels) ───────────────────────────────────────────

class SingleGauge {
public:
    SingleGauge(std::string name, std::string help)
        : name_(std::move(name)), help_(std::move(help)) {}

    void Set(int64_t v) noexcept { value_.store(v, std::memory_order_relaxed); }

    void Visit(const SampleVisitor& fn) const {
        MetricSample s;
        s.type  = MetricType::Gauge;
        s.name  = name_;
        s.help  = help_;
        s.value = value_.load(std::memory_order_relaxed);
        fn(s);
    }

    const std::string& name() const noexcept { return name_; }

private:
    std::string          name_;
    std::string          help_;
    std::atomic<int64_t> value_{0};
};

// ─── Render helpers (used by exposer.cc & tests) ──────────────────────────────

// Escape a label value per Prometheus exposition format (`\`, `"`, `\n`).
std::string EscapeLabelValue(std::string_view v);

// Append "# HELP", "# TYPE", and one or more sample lines for a single
// MetricSample into `out`. For Summary, emits per-quantile lines + _count + _sum.
// Skips HELP/TYPE if the metric name has already been emitted (the caller is
// expected to gate that via a set; this helper does NOT track state itself).
void RenderSample(const MetricSample& s, std::string* out);

// Helper to emit the "# HELP / # TYPE" header for a metric family.
void RenderHelpType(const MetricSample& s, std::string* out);

// Helper to emit only the sample line(s), without HELP/TYPE.
void RenderSampleLines(const MetricSample& s, std::string* out);

}  // namespace yikv_server::metrics
