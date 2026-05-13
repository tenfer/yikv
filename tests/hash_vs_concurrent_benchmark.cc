// HashMap (SWMR + FtAllocator SingleWriter) vs ConcurrentHashMap
// (FtAllocator Concurrent) vs std::unordered_map（堆分配；线程安全靠 mutex / shared_mutex）.
//
// 场景对照（运行: bazel run -c opt //tests:hash_vs_concurrent_benchmark -- ...）:
//   1) SingleWriterPut* / StdUnorderedMap_SingleThreadPut
//   2) ConcurrentHashMap_*WriterPut* / StdUnorderedMap_ParallelWritersMutex*（全局互斥写）
//   3) *MultiRead* / StdUnorderedMap_MultiRead_SharedLock（共享锁读）
//   4–5) 混合 / StdUnorderedMap_1Writer4Readers_Mixed
//
// 默认 --benchmark_min_time=0.2s；可加 --benchmark_filter='...' 筛选。

#include "src/alloc/allocator.h"
#include "src/alloc/ft_allocator.h"
#include "src/container/concurrent_hashmap.h"
#include "src/container/hashmap.h"

#include <benchmark/benchmark.h>

#include <cstdint>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>

namespace {

using yikv::alloc::AllocatorMode;
using yikv::alloc::AllocatorOptions;
using yikv::alloc::FtAllocator;
using yikv::container::ConcurrentHashMap;
using yikv::container::HashMap;

// ~128k 条预热键；写基准里循环写避免 arena 爆掉
constexpr uint32_t kBucketBitsCHM = 17;
constexpr uint64_t kWriteMod      = 65536;
constexpr uint64_t kReadSpan      = 131072;
constexpr uint64_t kMixedPreload  = 65536;
// 混合场景每轮状态迭代内的 put/get 次数（摊薄 HashMap publish / snapshot 成本，避免 arena 爆）
constexpr int kMixedBatch = 64;

// 各线程只上报本线程完成的 put 或 get 次数；Google Benchmark 对同名 counter 跨线程求和后换算速率。
inline void mixed_rw_counters(benchmark::State& state, bool is_writer_thread) {
    const double n =
        static_cast<double>(state.iterations()) * static_cast<double>(kMixedBatch);
    state.counters["mixed_writes"] =
        benchmark::Counter(is_writer_thread ? n : 0.0, benchmark::Counter::kIsRate);
    state.counters["mixed_reads"] =
        benchmark::Counter(is_writer_thread ? 0.0 : n, benchmark::Counter::kIsRate);
}

// std::unordered_map + shared_mutex（多读基准 / 混合读侧）
struct StdMapShard {
    std::unordered_map<std::uint64_t, std::uint64_t> m;
    mutable std::shared_mutex                       mx;
};

AllocatorOptions OptSw(std::uint64_t arena_mb = 512) {
    AllocatorOptions o;
    o.arena_size = arena_mb * 1024 * 1024;
    o.mode       = AllocatorMode::SingleWriter;
    return o;
}

AllocatorOptions OptSwGrow(std::uint64_t arena_mb, std::uint64_t max_arena_mb) {
    AllocatorOptions o = OptSw(arena_mb);
    o.segment_size   = arena_mb * 1024 * 1024;
    o.max_arena_size = max_arena_mb * 1024 * 1024;
    return o;
}

AllocatorOptions OptConcurrent(std::uint64_t arena_mb = 512) {
    AllocatorOptions o;
    o.arena_size = arena_mb * 1024 * 1024;
    o.mode       = AllocatorMode::Concurrent;
    return o;
}

AllocatorOptions OptConcurrentGrow(std::uint64_t arena_mb, std::uint64_t max_arena_mb) {
    AllocatorOptions o = OptConcurrent(arena_mb);
    o.segment_size   = arena_mb * 1024 * 1024;
    o.max_arena_size = max_arena_mb * 1024 * 1024;
    return o;
}

static void BM_HashMap_SingleWriterPut(benchmark::State& state) {
    FtAllocator a;
    a.Open(OptSw());
    HashMap<std::uint64_t, std::uint64_t> hm(&a);
    std::uint64_t i = 0;
    for (auto _ : state) {
        const std::uint64_t k = i++ % kWriteMod;
        hm.put(k, k ^ 0xAAAAAAAAu);
        hm.publish();
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_HashMap_SingleWriterPut)->Unit(benchmark::kMicrosecond);

static void BM_ConcurrentHashMap_SingleWriterPut(benchmark::State& state) {
    FtAllocator a;
    a.Open(OptConcurrent());
    ConcurrentHashMap<std::uint64_t, std::uint64_t> hm(&a, 0, kBucketBitsCHM, 6);
    std::uint64_t i = 0;
    for (auto _ : state) {
        const std::uint64_t k = i++ % kWriteMod;
        hm.put(k, k ^ 0xAAAAAAAAu);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ConcurrentHashMap_SingleWriterPut)->Unit(benchmark::kMicrosecond);

static void BM_StdUnorderedMap_SingleThreadPut(benchmark::State& state) {
    std::unordered_map<std::uint64_t, std::uint64_t> m;
    m.reserve(static_cast<std::size_t>(kWriteMod * 2));
    std::uint64_t i = 0;
    for (auto _ : state) {
        const std::uint64_t k = i++ % kWriteMod;
        m[k] = k ^ 0xAAAAAAAAu;
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_StdUnorderedMap_SingleThreadPut)->Unit(benchmark::kMicrosecond);

static void BM_ConcurrentHashMap_2WritersPut(benchmark::State& state) {
    FtAllocator a;
    a.Open(OptConcurrent());
    ConcurrentHashMap<std::uint64_t, std::uint64_t> hm(&a, 0, kBucketBitsCHM, 6);
    const int        tid = static_cast<int>(state.thread_index());
    std::uint64_t i      = 0;
    for (auto _ : state) {
        const std::uint64_t k =
            (static_cast<std::uint64_t>(tid) + i * static_cast<std::uint64_t>(state.threads())) %
            kWriteMod;
        hm.put(k, i ^ 0xBBBBBBBBu);
        ++i;
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ConcurrentHashMap_2WritersPut)
    ->Threads(2)
    ->Unit(benchmark::kMicrosecond);

static void BM_ConcurrentHashMap_4WritersPut(benchmark::State& state) {
    FtAllocator a;
    a.Open(OptConcurrent());
    ConcurrentHashMap<std::uint64_t, std::uint64_t> hm(&a, 0, kBucketBitsCHM, 6);
    const int        tid = static_cast<int>(state.thread_index());
    std::uint64_t i      = 0;
    for (auto _ : state) {
        const std::uint64_t k =
            (static_cast<std::uint64_t>(tid) + i * static_cast<std::uint64_t>(state.threads())) %
            kWriteMod;
        hm.put(k, i ^ 0xCCCCCCCCu);
        ++i;
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ConcurrentHashMap_4WritersPut)
    ->Threads(4)
    ->Unit(benchmark::kMicrosecond);

// 多写伸缩：各线程键空间错开 (tid + i*T) % kWriteMod；32 线程时默认 64 条带竞争加剧，
// 可对照 *Stripe8（256 条带）。
static void BM_ConcurrentHashMap_ParallelWritersPut(benchmark::State& state) {
    FtAllocator a;
    a.Open(OptConcurrentGrow(512, 8192));
    ConcurrentHashMap<std::uint64_t, std::uint64_t> hm(&a, 0, kBucketBitsCHM, 6);
    const int        tid = static_cast<int>(state.thread_index());
    const int        T   = static_cast<int>(state.threads());
    std::uint64_t i      = 0;
    for (auto _ : state) {
        const std::uint64_t k =
            (static_cast<std::uint64_t>(tid) + i * static_cast<std::uint64_t>(T)) %
            kWriteMod;
        hm.put(k, i ^ 0xE1E1E1E1u);
        ++i;
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ConcurrentHashMap_ParallelWritersPut)
    ->Threads(8)
    ->Unit(benchmark::kMicrosecond);
BENCHMARK(BM_ConcurrentHashMap_ParallelWritersPut)
    ->Threads(16)
    ->Unit(benchmark::kMicrosecond);
BENCHMARK(BM_ConcurrentHashMap_ParallelWritersPut)
    ->Threads(32)
    ->Unit(benchmark::kMicrosecond);

static void BM_ConcurrentHashMap_ParallelWritersPut_Stripe8(benchmark::State& state) {
    FtAllocator a;
    a.Open(OptConcurrentGrow(512, 8192));
    ConcurrentHashMap<std::uint64_t, std::uint64_t> hm(&a, 0, kBucketBitsCHM,
                                                      /*stripe_shift=*/8);
    const int        tid = static_cast<int>(state.thread_index());
    const int        T   = static_cast<int>(state.threads());
    std::uint64_t i      = 0;
    for (auto _ : state) {
        const std::uint64_t k =
            (static_cast<std::uint64_t>(tid) + i * static_cast<std::uint64_t>(T)) %
            kWriteMod;
        hm.put(k, i ^ 0xF2F2F2F2u);
        ++i;
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ConcurrentHashMap_ParallelWritersPut_Stripe8)
    ->Threads(32)
    ->Unit(benchmark::kMicrosecond);

// 多写：std unordered_map 非线程安全，使用全局 mutex 包裹每次 insert/赋值。
static void BM_StdUnorderedMap_ParallelWritersMutex(benchmark::State& state) {
    std::unordered_map<std::uint64_t, std::uint64_t> m;
    std::mutex                                       mu;
    m.reserve(static_cast<std::size_t>(kWriteMod * 4));
    const int        tid = static_cast<int>(state.thread_index());
    const int        T   = static_cast<int>(state.threads());
    std::uint64_t i      = 0;
    for (auto _ : state) {
        const std::uint64_t k =
            (static_cast<std::uint64_t>(tid) + i * static_cast<std::uint64_t>(T)) %
            kWriteMod;
        std::lock_guard<std::mutex> lk(mu);
        m[k] = i ^ 0xD0D0D0D0u;
        ++i;
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_StdUnorderedMap_ParallelWritersMutex)
    ->Threads(2)
    ->Unit(benchmark::kMicrosecond);
BENCHMARK(BM_StdUnorderedMap_ParallelWritersMutex)
    ->Threads(4)
    ->Unit(benchmark::kMicrosecond);
BENCHMARK(BM_StdUnorderedMap_ParallelWritersMutex)
    ->Threads(8)
    ->Unit(benchmark::kMicrosecond);
BENCHMARK(BM_StdUnorderedMap_ParallelWritersMutex)
    ->Threads(16)
    ->Unit(benchmark::kMicrosecond);
BENCHMARK(BM_StdUnorderedMap_ParallelWritersMutex)
    ->Threads(32)
    ->Unit(benchmark::kMicrosecond);

static void BM_HashMap_MultiRead(benchmark::State& state) {
    FtAllocator a;
    a.Open(OptSw());
    HashMap<std::uint64_t, std::uint64_t> hm(&a);
    for (std::uint64_t k = 0; k < kReadSpan; ++k) {
        hm.put(k, k * 3 + 1);
    }
    hm.publish();
    auto snap = hm.acquire_snapshot();

    const int  tid = static_cast<int>(state.thread_index());
    std::uint64_t i = 0;
    for (auto _ : state) {
        const std::uint64_t k =
            (i * 0x9e3779b97f4a7c15ULL + static_cast<std::uint64_t>(tid)) % kReadSpan;
        std::uint64_t v = 0;
        benchmark::DoNotOptimize(snap.get(k, v));
        benchmark::DoNotOptimize(v);
        ++i;
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_HashMap_MultiRead)->ThreadRange(1, 8)->Unit(benchmark::kMicrosecond);

static void BM_ConcurrentHashMap_MultiRead(benchmark::State& state) {
    FtAllocator a;
    a.Open(OptConcurrent());
    ConcurrentHashMap<std::uint64_t, std::uint64_t> hm(&a, 0, kBucketBitsCHM, 6);
    for (std::uint64_t k = 0; k < kReadSpan; ++k) {
        hm.put(k, k * 3 + 1);
    }

    const int  tid = static_cast<int>(state.thread_index());
    std::uint64_t i = 0;
    for (auto _ : state) {
        const std::uint64_t k =
            (i * 0x9e3779b97f4a7c15ULL + static_cast<std::uint64_t>(tid)) % kReadSpan;
        std::uint64_t v = 0;
        benchmark::DoNotOptimize(hm.get(k, v));
        benchmark::DoNotOptimize(v);
        ++i;
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ConcurrentHashMap_MultiRead)->ThreadRange(1, 8)->Unit(benchmark::kMicrosecond);

static void BM_ConcurrentHashMap_MultiRead_LockFree(benchmark::State& state) {
    FtAllocator a;
    a.Open(OptConcurrent());
    ConcurrentHashMap<std::uint64_t, std::uint64_t> hm(&a, 0, kBucketBitsCHM, 6);
    for (std::uint64_t k = 0; k < kReadSpan; ++k) {
        hm.put(k, k * 3 + 1);
    }
    hm.enable_lock_free_reads();

    const int  tid = static_cast<int>(state.thread_index());
    std::uint64_t i = 0;
    for (auto _ : state) {
        const std::uint64_t k =
            (i * 0x9e3779b97f4a7c15ULL + static_cast<std::uint64_t>(tid)) % kReadSpan;
        std::uint64_t v = 0;
        benchmark::DoNotOptimize(hm.get(k, v));
        benchmark::DoNotOptimize(v);
        ++i;
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ConcurrentHashMap_MultiRead_LockFree)
    ->ThreadRange(1, 8)
    ->Unit(benchmark::kMicrosecond);

// 并发只读：shared_mutex 共享锁 + find（无写竞争时等价于读者并行）。
static void BM_StdUnorderedMap_MultiRead_SharedLock(benchmark::State& state) {
    auto sm = std::make_unique<StdMapShard>();
    sm->m.reserve(static_cast<std::size_t>(kReadSpan * 2));
    for (std::uint64_t k = 0; k < kReadSpan; ++k) {
        sm->m[k] = k * 3 + 1;
    }

    const int  tid = static_cast<int>(state.thread_index());
    std::uint64_t i = 0;
    for (auto _ : state) {
        const std::uint64_t k =
            (i * 0x9e3779b97f4a7c15ULL + static_cast<std::uint64_t>(tid)) % kReadSpan;
        std::shared_lock lk(sm->mx);
        auto                it = sm->m.find(k);
        benchmark::DoNotOptimize(it);
        const std::uint64_t v = (it != sm->m.end()) ? it->second : 0;
        benchmark::DoNotOptimize(&v);
        ++i;
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_StdUnorderedMap_MultiRead_SharedLock)
    ->ThreadRange(1, 8)
    ->Unit(benchmark::kMicrosecond);

// 1 写 + 4 读；Hash 读者每轮 snapshot+get，CHM 每轮 get。写线程更新独立槽位，读者扫前 1/2 键。
static void BM_HashMap_1Writer4Readers_Mixed(benchmark::State& state) {
    if (state.threads() != 5) {
        state.SkipWithError("Need exactly 5 threads (1 writer + 4 readers)");
        return;
    }
    FtAllocator a;
    a.Open(OptSwGrow(/*arena_mb=*/2048, /*max_arena_mb=*/8192));
    HashMap<std::uint64_t, std::uint64_t> hm(&a);
    for (std::uint64_t k = 0; k < kMixedPreload; ++k) {
        hm.put(k, k);
    }
    hm.publish();

    const int        tid = static_cast<int>(state.thread_index());
    std::uint64_t i      = 0;
    for (auto _ : state) {
        if (tid == 0) {
            for (int b = 0; b < kMixedBatch; ++b) {
                const std::uint64_t k = kMixedPreload + ((i + static_cast<std::uint64_t>(b)) % kWriteMod);
                hm.put(k, i + static_cast<std::uint64_t>(b));
            }
            hm.publish();
        } else {
            auto snap = hm.acquire_snapshot();
            for (int b = 0; b < kMixedBatch; ++b) {
                const std::uint64_t k =
                    ((i + static_cast<std::uint64_t>(b)) * 0xd6e8feb866449d7fULL +
                     static_cast<std::uint64_t>(tid)) %
                    (kMixedPreload / 2);
                std::uint64_t v = 0;
                benchmark::DoNotOptimize(snap.get(k, v));
                benchmark::DoNotOptimize(v);
            }
        }
        i += static_cast<std::uint64_t>(kMixedBatch);
    }
    mixed_rw_counters(state, tid == 0);
}
BENCHMARK(BM_HashMap_1Writer4Readers_Mixed)
    ->Threads(5)
    ->Iterations(2000)
    ->Unit(benchmark::kMicrosecond);

static void BM_ConcurrentHashMap_1Writer4Readers_Mixed(benchmark::State& state) {
    if (state.threads() != 5) {
        state.SkipWithError("Need exactly 5 threads");
        return;
    }
    FtAllocator a;
    a.Open(OptConcurrentGrow(/*arena_mb=*/512, /*max_arena_mb=*/8192));
    ConcurrentHashMap<std::uint64_t, std::uint64_t> hm(&a, 0, kBucketBitsCHM, 6);
    for (std::uint64_t k = 0; k < kMixedPreload; ++k) {
        hm.put(k, k);
    }

    const int        tid = static_cast<int>(state.thread_index());
    std::uint64_t i      = 0;
    for (auto _ : state) {
        if (tid == 0) {
            for (int b = 0; b < kMixedBatch; ++b) {
                const std::uint64_t k = kMixedPreload + ((i + static_cast<std::uint64_t>(b)) % kWriteMod);
                hm.put(k, i + static_cast<std::uint64_t>(b));
            }
        } else {
            for (int b = 0; b < kMixedBatch; ++b) {
                const std::uint64_t k =
                    ((i + static_cast<std::uint64_t>(b)) * 0xd6e8feb866449d7fULL +
                     static_cast<std::uint64_t>(tid)) %
                    (kMixedPreload / 2);
                std::uint64_t v = 0;
                benchmark::DoNotOptimize(hm.get(k, v));
                benchmark::DoNotOptimize(v);
            }
        }
        i += static_cast<std::uint64_t>(kMixedBatch);
    }
    mixed_rw_counters(state, tid == 0);
}
BENCHMARK(BM_ConcurrentHashMap_1Writer4Readers_Mixed)->Threads(5)->Unit(benchmark::kMicrosecond);

// std::unordered_map 1W4R：每个 put / find 各加一次锁（全局 rwlock vs CHM 分段锁 + arena）。
static void BM_StdUnorderedMap_1Writer4Readers_Mixed(benchmark::State& state) {
    if (state.threads() != 5) {
        state.SkipWithError("Need exactly 5 threads");
        return;
    }
    auto sm = std::make_unique<StdMapShard>();
    {
        std::unique_lock lk(sm->mx);
        sm->m.reserve(static_cast<std::size_t>(kMixedPreload + kWriteMod));
        for (std::uint64_t k = 0; k < kMixedPreload; ++k) {
            sm->m[k] = k;
        }
    }

    const int        tid = static_cast<int>(state.thread_index());
    std::uint64_t i      = 0;
    for (auto _ : state) {
        if (tid == 0) {
            for (int b = 0; b < kMixedBatch; ++b) {
                const std::uint64_t key = kMixedPreload + ((i + static_cast<std::uint64_t>(b)) % kWriteMod);
                std::unique_lock lk(sm->mx);
                sm->m[key] = i + static_cast<std::uint64_t>(b);
            }
        } else {
            for (int b = 0; b < kMixedBatch; ++b) {
                const std::uint64_t key =
                    ((i + static_cast<std::uint64_t>(b)) * 0xd6e8feb866449d7fULL +
                     static_cast<std::uint64_t>(tid)) %
                    (kMixedPreload / 2);
                std::shared_lock lk(sm->mx);
                auto                it = sm->m.find(key);
                benchmark::DoNotOptimize(it);
                const std::uint64_t v = (it != sm->m.end()) ? it->second : 0;
                benchmark::DoNotOptimize(&v);
            }
        }
        i += static_cast<std::uint64_t>(kMixedBatch);
    }
    mixed_rw_counters(state, tid == 0);
}
BENCHMARK(BM_StdUnorderedMap_1Writer4Readers_Mixed)->Threads(5)->Unit(benchmark::kMicrosecond);

static void BM_ConcurrentHashMap_2Writers4Readers_Mixed(benchmark::State& state) {
    if (state.threads() != 6) {
        state.SkipWithError("Need exactly 6 threads (2 writers + 4 readers)");
        return;
    }
    FtAllocator a;
    a.Open(OptConcurrentGrow(/*arena_mb=*/512, /*max_arena_mb=*/8192));
    ConcurrentHashMap<std::uint64_t, std::uint64_t> hm(&a, 0, kBucketBitsCHM, 6);
    for (std::uint64_t k = 0; k < kMixedPreload; ++k) {
        hm.put(k, k);
    }

    const int        tid = static_cast<int>(state.thread_index());
    std::uint64_t i      = 0;
    for (auto _ : state) {
        if (tid <= 1) {
            for (int b = 0; b < kMixedBatch; ++b) {
                const std::uint64_t k =
                    kMixedPreload +
                    ((i + static_cast<std::uint64_t>(b)) % (kWriteMod / 2)) +
                    static_cast<std::uint64_t>(tid) * 8192;
                hm.put(k, i + static_cast<std::uint64_t>(b));
            }
        } else {
            for (int b = 0; b < kMixedBatch; ++b) {
                const std::uint64_t k =
                    ((i + static_cast<std::uint64_t>(b)) * 0xd6e8feb866449d7fULL +
                     static_cast<std::uint64_t>(tid)) %
                    (kMixedPreload / 2);
                std::uint64_t v = 0;
                benchmark::DoNotOptimize(hm.get(k, v));
                benchmark::DoNotOptimize(v);
            }
        }
        i += static_cast<std::uint64_t>(kMixedBatch);
    }
    mixed_rw_counters(state, tid <= 1);
}
BENCHMARK(BM_ConcurrentHashMap_2Writers4Readers_Mixed)->Threads(6)->Unit(benchmark::kMicrosecond);

static void BM_ConcurrentHashMap_4Writers4Readers_Mixed(benchmark::State& state) {
    if (state.threads() != 8) {
        state.SkipWithError("Need exactly 8 threads (4 writers + 4 readers)");
        return;
    }
    FtAllocator a;
    a.Open(OptConcurrentGrow(/*arena_mb=*/512, /*max_arena_mb=*/8192));
    ConcurrentHashMap<std::uint64_t, std::uint64_t> hm(&a, 0, kBucketBitsCHM, 6);
    for (std::uint64_t k = 0; k < kMixedPreload; ++k) {
        hm.put(k, k);
    }

    const int        tid = static_cast<int>(state.thread_index());
    std::uint64_t i      = 0;
    for (auto _ : state) {
        if (tid <= 3) {
            for (int b = 0; b < kMixedBatch; ++b) {
                const std::uint64_t k =
                    kMixedPreload +
                    ((i + static_cast<std::uint64_t>(b)) % (kWriteMod / 4)) +
                    static_cast<std::uint64_t>(tid) * 4096;
                hm.put(k, i + static_cast<std::uint64_t>(b));
            }
        } else {
            for (int b = 0; b < kMixedBatch; ++b) {
                const std::uint64_t k =
                    ((i + static_cast<std::uint64_t>(b)) * 0xd6e8feb866449d7fULL +
                     static_cast<std::uint64_t>(tid)) %
                    (kMixedPreload / 2);
                std::uint64_t v = 0;
                benchmark::DoNotOptimize(hm.get(k, v));
                benchmark::DoNotOptimize(v);
            }
        }
        i += static_cast<std::uint64_t>(kMixedBatch);
    }
    mixed_rw_counters(state, tid <= 3);
}
BENCHMARK(BM_ConcurrentHashMap_4Writers4Readers_Mixed)->Threads(8)->Unit(benchmark::kMicrosecond);

}  // namespace
