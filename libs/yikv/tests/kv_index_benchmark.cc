#include "index/kv_index.h"
#include "alloc/ft_allocator.h"
#include "schema/schema.h"

#include <benchmark/benchmark.h>

#include <string>

// These benchmarks use a fixed-size mmap arena (FtAllocator).
// FtAllocator now reclaims Delayed-freed blocks automatically inside Malloc()
// (every 256 allocs, configurable via reclaim_delay_ns).  Upsert cases that
// replace the same key benefit from transparent Doc/HashMap-node reclamation
// and no longer accumulate unbounded arena usage.
// PutUnique still inserts unique keys each iteration, so the arena fills up
// proportionally; Iterations(...) caps the run to avoid std::bad_alloc.

namespace {

using yikv::alloc::AllocatorOptions;
using yikv::alloc::FtAllocator;
using yikv::index::Doc;
using yikv::index::KVIndex;
using yikv::schema::Schema;

// Same shape as kv_index_test: int64 pk, int32 age, string name, int64[] clk_list.
static const char* kBenchSchemaJson = R"({
  "table_name": "bench_user",
  "pk": "user_id",
  "fields": [
    {"name": "user_id", "data_type": "int64", "is_pk": true,  "is_index": false, "field_id": 1},
    {"name": "age",     "data_type": "int32", "is_pk": false, "is_index": false, "field_id": 2},
    {"name": "name",    "data_type": "string","is_pk": false, "is_index": false, "field_id": 3},
    {"name": "clk_list","data_type": "int64", "is_pk": false, "is_index": false, "field_id": 4,
     "is_array": true}
  ]
})";

static constexpr uint32_t kFidUserId = 1;
static constexpr uint32_t kFidAge    = 2;
static constexpr uint32_t kFidName   = 3;

static AllocatorOptions BenchArena(std::uint64_t arena_mb = 512) {
    AllocatorOptions opts;
    opts.arena_size = arena_mb * 1024 * 1024;
    return opts;
}

// Optional extra virtual capacity for segmented growth (see FtAllocator); avoids hard
// failures when a single benchmark needs more than arena_size during setup + timed loop.
static AllocatorOptions BenchArenaGrow(std::uint64_t arena_mb,
                                       std::uint64_t max_arena_mb) {
    AllocatorOptions opts = BenchArena(arena_mb);
    opts.segment_size   = arena_mb * 1024 * 1024;
    opts.max_arena_size = max_arena_mb * 1024 * 1024;
    return opts;
}

static bool LoadSchema(Schema* schema) {
    std::string err;
    return schema->LoadJson(kBenchSchemaJson, &err);
}

// Insert a new row each iteration (unique pk). Measures Put + publish + NewDoc field writes.
static void BM_KVIndex_PutUnique(benchmark::State& state) {
    FtAllocator alloc;
    const std::uint64_t mb = static_cast<std::uint64_t>(state.range(0));
    alloc.Open(BenchArenaGrow(/*arena_mb=*/mb, /*max_arena_mb=*/mb * 4));
    Schema schema;
    if (!LoadSchema(&schema)) {
        state.SkipWithError("schema load failed");
        return;
    }
    KVIndex idx(&alloc, &schema);

    std::int64_t next_key = 1;
    for (auto _ : state) {
        Doc d = idx.NewDoc();
        d.put_int64(kFidUserId, next_key++);
        d.put_int32(kFidAge, 28);
        d.put_string(kFidName, "benchmark");
        idx.Put(&d);
    }
    state.SetItemsProcessed(state.iterations());
}
// Fixed iteration count: each iter allocates; avoid unbounded inner loops under min_time.
BENCHMARK(BM_KVIndex_PutUnique)
    ->Arg(512)
    ->Iterations(40000)
    ->Unit(benchmark::kMicrosecond);

// Repeated lookup of one existing key.
static void BM_KVIndex_GetHit(benchmark::State& state) {
    FtAllocator alloc;
    alloc.Open(BenchArena(512));
    Schema schema;
    if (!LoadSchema(&schema)) {
        state.SkipWithError("schema load failed");
        return;
    }
    KVIndex idx(&alloc, &schema);

    Doc seed = idx.NewDoc();
    seed.put_int64(kFidUserId, 424242);
    seed.put_int32(kFidAge, 99);
    seed.put_string(kFidName, "hot_row");
    idx.Put(&seed);

    Doc out;
    for (auto _ : state) {
        benchmark::DoNotOptimize(idx.Get("424242", &out));
        benchmark::DoNotOptimize(out.get_int64(kFidUserId));
        benchmark::DoNotOptimize(out.get_int32(kFidAge));
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_KVIndex_GetHit)->Unit(benchmark::kMicrosecond);

// Lookup cost grows with table size; range arg = number of rows inserted before timing.
static void BM_KVIndex_GetHitAtScale(benchmark::State& state) {
    const int n = static_cast<int>(state.range(0));
    FtAllocator alloc;
    alloc.Open(BenchArena(2048));
    Schema schema;
    if (!LoadSchema(&schema)) {
        state.SkipWithError("schema load failed");
        return;
    }
    KVIndex idx(&alloc, &schema);

    for (int i = 0; i < n; ++i) {
        Doc d = idx.NewDoc();
        d.put_int64(kFidUserId, static_cast<std::int64_t>(i));
        d.put_int32(kFidAge, i & 255);
        d.put_string(kFidName, "row");
        idx.Put(&d);
    }

    const std::string target_pk = std::to_string(n / 2);
    Doc out;
    for (auto _ : state) {
        benchmark::DoNotOptimize(idx.Get(target_pk, &out));
        benchmark::DoNotOptimize(out.get_int64(kFidUserId));
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_KVIndex_GetHitAtScale)->Arg(100)->Arg(1000)->Arg(10000)->Unit(benchmark::kMicrosecond);

// Upsert: replace same primary key each iteration (new Doc blob each time).
static void BM_KVIndex_PutUpsertSameKey(benchmark::State& state) {
    FtAllocator alloc;
    alloc.Open(BenchArenaGrow(512, 4096));
    Schema schema;
    if (!LoadSchema(&schema)) {
        state.SkipWithError("schema load failed");
        return;
    }
    KVIndex idx(&alloc, &schema);

    std::int64_t version = 0;
    for (auto _ : state) {
        Doc d = idx.NewDoc();
        d.put_int64(kFidUserId, 1);
        d.put_int32(kFidAge, static_cast<std::int32_t>(version++ & 0x7fffffff));
        d.put_string(kFidName, "upsert");
        idx.Put(&d);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_KVIndex_PutUpsertSameKey)
    ->Iterations(40000)
    ->Unit(benchmark::kMicrosecond);

}  // namespace
