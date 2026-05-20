// Exercises the yikv::db::DB singleton with real temp directories: Put throughput,
// inverted put+query, and cold OpenIndex + scan (stability of on-disk metadata + arena).
//
// Run: bazel run -c opt //tests:db_benchmark -- --benchmark_min_time=0.1s
// List: bazel run -c opt //tests:db_benchmark -- --benchmark_list_tests

#include "db/db.h"

#include "alloc/allocator.h"
#include "container/bitmap.h"
#include "index/inverted_index.h"
#include "index/kv_index.h"
#include "schema/schema.h"

#include <benchmark/benchmark.h>

#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

namespace {

using yikv::alloc::AllocatorOptions;
using yikv::db::DB;
using yikv::db::DBOptions;
using yikv::index::Doc;
using yikv::index::InvertedIndex;
using yikv::index::KVIndex;
using yikv::schema::Schema;

static const char* kBenchKVSchemaJson = R"({
  "table_name": "bench_kv",
  "pk": "user_id",
  "fields": [
    {"name": "user_id", "data_type": "int64", "is_pk": true,  "is_index": false, "field_id": 1},
    {"name": "age",     "data_type": "int32", "is_pk": false, "is_index": false, "field_id": 2},
    {"name": "name",    "data_type": "string","is_pk": false, "is_index": false, "field_id": 3}
  ]
})";

static const char* kBenchInvSchemaJson = R"({
  "table_name": "bench_inv",
  "pk": "user_id",
  "fields": [
    {"name": "user_id", "data_type": "int64", "is_pk": true,  "is_index": false, "field_id": 1},
    {"name": "bio",     "data_type": "string","is_pk": false, "is_index": true,  "field_id": 2}
  ]
})";

constexpr uint32_t kFidUserId = 1;
constexpr uint32_t kFidAge    = 2;
constexpr uint32_t kFidName   = 3;
constexpr uint32_t kFidBio    = 2;

std::string MakeTempDbRoot() {
    namespace fs = std::filesystem;
    std::string base =
        (fs::temp_directory_path() / "yikv_db_bench_XXXXXX").string();
    std::vector<char> buf(base.begin(), base.end());
    buf.push_back('\0');
    if (::mkdtemp(buf.data()) == nullptr) {
        throw std::runtime_error("mkdtemp failed");
    }
    return std::string(buf.data());
}

static AllocatorOptions BenchArenaGrow(std::uint64_t arena_mb, std::uint64_t max_arena_mb) {
    AllocatorOptions opts;
    opts.arena_size      = arena_mb * 1024 * 1024;
    opts.segment_size    = arena_mb * 1024 * 1024;
    opts.max_arena_size  = max_arena_mb * 1024 * 1024;
    return opts;
}

static bool LoadSchema(Schema* s, const char* json) {
    std::string err;
    return s->LoadJson(json, &err);
}

// Through DB::CreateKVIndex: unique PK each iteration, Put + Publish.
static void BM_DB_KV_PutUnique(benchmark::State& state) {
    std::string root = MakeTempDbRoot();
    DB::ResetForTest();
    DBOptions opt;
    opt.db_path          = root;
    opt.alloc_defaults   = BenchArenaGrow(/*arena_mb=*/512, /*max_arena_mb=*/4096);
    DB::Init(std::move(opt));

    Schema schema;
    if (!LoadSchema(&schema, kBenchKVSchemaJson)) {
        state.SkipWithError("schema");
        return;
    }
    DB::Instance().CreateKVIndex("kv", schema);
    KVIndex* idx = DB::Instance().GetKVIndex("kv");

    std::int64_t next_key = 1;
    for (auto _ : state) {
        Doc d = idx->NewDoc();
        d.put_int64(kFidUserId, next_key++);
        d.put_int32(kFidAge, 28);
        d.put_string(kFidName, "benchmark");
        idx->Put(&d);
        idx->Publish();
    }

    DB::Instance().CloseAll();
    DB::ResetForTest();
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_DB_KV_PutUnique)->Iterations(30000)->Unit(benchmark::kMicrosecond);

// Inverted: Put one doc (two tokens) + single-term Query per iteration.
static void BM_DB_Inverted_PutAndQuery(benchmark::State& state) {
    std::string root = MakeTempDbRoot();
    DB::ResetForTest();
    DBOptions opt;
    opt.db_path        = root;
    opt.alloc_defaults = BenchArenaGrow(512, 4096);
    DB::Init(std::move(opt));

    Schema schema;
    if (!LoadSchema(&schema, kBenchInvSchemaJson)) {
        state.SkipWithError("schema");
        return;
    }
    DB::Instance().CreateInvertedIndex("inv", schema);
    InvertedIndex* idx = DB::Instance().GetInvertedIndex("inv");

    std::int64_t pk = 0;
    for (auto _ : state) {
        Doc d = idx->NewDoc();
        uint32_t did = d.doc_id();
        d.put_int64(kFidUserId, ++pk);
        d.put_string(kFidBio, "alpha beta gamma");
        idx->Put(&d);

        yikv::container::Bitmap bm(idx->alloc(), 0);
        benchmark::DoNotOptimize(idx->Query(kFidBio, "beta", &bm));
        benchmark::DoNotOptimize(bm.Contains(did));
    }

    DB::Instance().CloseAll();
    DB::ResetForTest();
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_DB_Inverted_PutAndQuery)->Iterations(15000)->Unit(benchmark::kMicrosecond);

// Cold recovery: pre-fill index on disk once, then each iteration re-Init, OpenIndex,
// and scan all keys (measures mmap open + HashMap snapshot + Gets).
static void BM_DB_ColdOpenScan(benchmark::State& state) {
    const int kRows = static_cast<int>(state.range(0));
    std::string root = MakeTempDbRoot();

    {
        DB::ResetForTest();
        DBOptions opt;
        opt.db_path        = root;
        opt.alloc_defaults = BenchArenaGrow(512, 8192);
        DB::Init(std::move(opt));
        Schema schema;
        if (!LoadSchema(&schema, kBenchKVSchemaJson)) {
            state.SkipWithError("schema");
            std::filesystem::remove_all(root);
            return;
        }
        DB::Instance().CreateKVIndex("k", schema);
        KVIndex* idx = DB::Instance().GetKVIndex("k");
        for (int i = 1; i <= kRows; ++i) {
            Doc d = idx->NewDoc();
            d.put_int64(kFidUserId, static_cast<std::int64_t>(i));
            d.put_int32(kFidAge, i & 255);
            d.put_string(kFidName, "row");
            idx->Put(&d);
        }
        idx->Publish();
        DB::Instance().CloseAll();
        DB::ResetForTest();
    }

    for (auto _ : state) {
        DB::ResetForTest();
        DBOptions opt;
        opt.db_path        = root;
        opt.alloc_defaults = BenchArenaGrow(512, 8192);
        DB::Init(std::move(opt));
        DB::Instance().OpenIndex("k");
        KVIndex* idx = DB::Instance().GetKVIndex("k");

        std::uint64_t sum = 0;
        for (int i = 1; i <= kRows; ++i) {
            Doc out;
            if (idx->Get(std::to_string(i), &out)) {
                sum += static_cast<std::uint64_t>(out.get_int64(kFidUserId));
            }
        }
        benchmark::DoNotOptimize(sum);

        DB::Instance().CloseAll();
        DB::ResetForTest();
    }

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(kRows));
}
BENCHMARK(BM_DB_ColdOpenScan)->Arg(200)->Arg(2000)->Unit(benchmark::kMicrosecond);

}  // namespace
