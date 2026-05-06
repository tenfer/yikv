// db_tool — populate /data/yikv-style DB with a wide-schema KV index (>5GB by default)
// and optional read / write / SWMR benchmarks (see src/db/db.md).
//
// Example:
//   sudo mkdir -p /data/yikv && sudo chown "$USER" /data/yikv
//   bazel run //src/db:db_tool -- fill --db /data/yikv --index kv_all --target-gb 6 --recreate
//   bazel run //src/db:db_tool -- bench-read --db /data/yikv --index kv_all --ops 200000
//   bazel run //src/db:db_tool -- bench-write --db /data/yikv --index kv_all --ops 50000
//   bazel run //src/db:db_tool -- bench-swmr --db /data/yikv --index kv_all --threads 8 --seconds 10

#include "src/db/db.h"

#include "src/alloc/allocator.h"
#include "src/index/doc.h"
#include "src/index/kv_index.h"
#include "src/schema/schema.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

using yikv::alloc::AllocatorMode;
using yikv::db::DB;
using yikv::db::DBOptions;
using yikv::index::Doc;
using yikv::index::KVIndex;
using yikv::schema::Schema;

namespace {

// All scalar types + bytes + arrays of each fixed scalar (matches schema DataType).
static const char* kWideSchemaJson = R"({
  "table_name": "kv_all_types",
  "pk": "id",
  "fields": [
    {"name": "id",        "data_type": "int64",   "is_pk": true,  "field_id": 1,
     "nullable": false, "is_index": false},
    {"name": "f_bool",    "data_type": "bool",    "field_id": 2},
    {"name": "f_i32",     "data_type": "int32",   "field_id": 3},
    {"name": "f_i64",     "data_type": "int64",   "field_id": 4},
    {"name": "f_f32",     "data_type": "float32", "field_id": 5},
    {"name": "f_f64",     "data_type": "float64", "field_id": 6},
    {"name": "f_str",     "data_type": "string",  "field_id": 7},
    {"name": "f_bytes",   "data_type": "bytes",   "field_id": 8},
    {"name": "arr_i32",   "data_type": "int32",   "is_array": true, "field_id": 9},
    {"name": "arr_i64",   "data_type": "int64",   "is_array": true, "field_id": 10},
    {"name": "arr_f32",   "data_type": "float32", "is_array": true, "field_id": 11},
    {"name": "arr_f64",   "data_type": "float64", "is_array": true, "field_id": 12}
  ]
}
)";

constexpr uint32_t kFidId    = 1;
constexpr uint32_t kFidBool  = 2;
constexpr uint32_t kFidI32  = 3;
constexpr uint32_t kFidI64  = 4;
constexpr uint32_t kFidF32  = 5;
constexpr uint32_t kFidF64  = 6;
constexpr uint32_t kFidStr  = 7;
constexpr uint32_t kFidBytes = 8;
constexpr uint32_t kFidAi32 = 9;
constexpr uint32_t kFidAi64 = 10;
constexpr uint32_t kFidAf32 = 11;
constexpr uint32_t kFidAf64 = 12;

struct Config {
    std::string db_path      = "/data/yikv";
    std::string index_name   = "kv_all";
    double      target_gb    = 5.5;
    bool        recreate     = false;
    uint64_t    ops          = 200'000;
    int         threads      = 8;
    int         seconds      = 10;
    uint64_t    arena_seg_gb = 1;
    uint64_t    arena_max_gb = 12;
};

void Usage() {
    std::cerr
        << "Usage:\n"
        << "  db_tool fill   [--db PATH] [--index NAME] [--target-gb N] [--recreate]\n"
        << "                 [--arena-seg-gb N] [--arena-max-gb N]\n"
        << "  db_tool bench-read  [--db PATH] [--index NAME] [--ops N]\n"
        << "  db_tool bench-write [--db PATH] [--index NAME] [--ops N]\n"
        << "  db_tool bench-swmr  [--db PATH] [--index NAME] [--threads N] [--seconds N]\n";
}

bool ParseArgs(int argc, char** argv, const char* subcmd, Config* cfg) {
    for (int i = 2; i < argc; ++i) {
        if (std::strcmp(argv[i], "--db") == 0 && i + 1 < argc) {
            cfg->db_path = argv[++i];
        } else if (std::strcmp(argv[i], "--index") == 0 && i + 1 < argc) {
            cfg->index_name = argv[++i];
        } else if (std::strcmp(argv[i], "--target-gb") == 0 && i + 1 < argc) {
            cfg->target_gb = std::strtod(argv[++i], nullptr);
        } else if (std::strcmp(argv[i], "--recreate") == 0) {
            cfg->recreate = true;
        } else if (std::strcmp(argv[i], "--ops") == 0 && i + 1 < argc) {
            cfg->ops = std::strtoull(argv[++i], nullptr, 10);
        } else if (std::strcmp(argv[i], "--threads") == 0 && i + 1 < argc) {
            cfg->threads = static_cast<int>(std::strtol(argv[++i], nullptr, 10));
        } else if (std::strcmp(argv[i], "--seconds") == 0 && i + 1 < argc) {
            cfg->seconds = static_cast<int>(std::strtol(argv[++i], nullptr, 10));
        } else if (std::strcmp(argv[i], "--arena-seg-gb") == 0 && i + 1 < argc) {
            cfg->arena_seg_gb = std::strtoull(argv[++i], nullptr, 10);
        } else if (std::strcmp(argv[i], "--arena-max-gb") == 0 && i + 1 < argc) {
            cfg->arena_max_gb = std::strtoull(argv[++i], nullptr, 10);
        } else {
            std::cerr << "Unknown arg: " << argv[i] << "\n";
            return false;
        }
    }
    (void)subcmd;
    return true;
}

DBOptions MakeDbOptions(const Config& c) {
    DBOptions opt;
    opt.db_path                    = c.db_path;
    opt.alloc_defaults.arena_size     = c.arena_seg_gb * 1024ull * 1024ull * 1024ull;
    opt.alloc_defaults.segment_size     = c.arena_seg_gb * 1024ull * 1024ull * 1024ull;
    opt.alloc_defaults.max_arena_size   = c.arena_max_gb * 1024ull * 1024ull * 1024ull;
    opt.alloc_defaults.mode             = AllocatorMode::SingleWriter;
    opt.alloc_defaults.reclaim_delay_ns = 0;  // fill: reclaim ASAP under churn
    return opt;
}

void FillBenchPayload(int64_t id, std::string* str, std::string* bytes,
                      std::vector<int32_t>* a32, std::vector<int64_t>* a64,
                      std::vector<float>* af32, std::vector<double>* af64) {
    // Small rows for write throughput tests (large FillLargeStrings would exceed
    // default max_arena_size when --ops is huge).
    constexpr size_t kStr   = 192;
    constexpr size_t kBytes = 192;

    str->resize(kStr);
    bytes->resize(kBytes);
    for (size_t i = 0; i < kStr; ++i) {
        (*str)[i] = static_cast<char>('A' + ((id + static_cast<int64_t>(i)) % 26));
    }
    for (size_t i = 0; i < kBytes; ++i) {
        (*bytes)[i] = static_cast<char>((id * 131 + static_cast<int>(i)) & 0xFF);
    }

    constexpr size_t kA32  = 48;
    constexpr size_t kA64  = 32;
    constexpr size_t kAf32 = 32;
    constexpr size_t kAf64 = 24;

    a32->resize(kA32);
    a64->resize(kA64);
    af32->resize(kAf32);
    af64->resize(kAf64);
    for (size_t i = 0; i < kA32; ++i)
        (*a32)[i] = static_cast<int32_t>(id + static_cast<int64_t>(i));
    for (size_t i = 0; i < kA64; ++i)
        (*a64)[i] = id * 10007 + static_cast<int64_t>(i);
    for (size_t i = 0; i < kAf32; ++i)
        (*af32)[i] = static_cast<float>(id + i) * 0.001f;
    for (size_t i = 0; i < kAf64; ++i)
        (*af64)[i] = static_cast<double>(id + i) * 0.0001;
}

void FillLargeStrings(int64_t id, std::string* str, std::string* bytes,
                      std::vector<int32_t>* a32, std::vector<int64_t>* a64,
                      std::vector<float>* af32, std::vector<double>* af64) {
    // Per-doc blobs sized so O(k) docs exceed target GB (HashMap + Doc overhead extra).
    constexpr size_t kStrTarget  = 340'000;
    constexpr size_t kBytesTarget = 340'000;
    constexpr size_t kA32 = 4096;
    constexpr size_t kA64 = 8192;
    constexpr size_t kAf32 = 8192;
    constexpr size_t kAf64 = 4096;

    str->resize(kStrTarget);
    bytes->resize(kBytesTarget);
    for (size_t i = 0; i < kStrTarget; ++i) {
        char base = static_cast<char>('A' + ((id + static_cast<int64_t>(i)) % 26));
        (*str)[i]  = base;
    }
    for (size_t i = 0; i < kBytesTarget; ++i) {
        (*bytes)[i] = static_cast<char>((id * 131 + static_cast<int>(i)) & 0xFF);
    }

    a32->resize(kA32);
    a64->resize(kA64);
    af32->resize(kAf32);
    af64->resize(kAf64);
    for (size_t i = 0; i < kA32; ++i)
        (*a32)[i] = static_cast<int32_t>(id + static_cast<int64_t>(i));
    for (size_t i = 0; i < kA64; ++i)
        (*a64)[i] = id * 10007 + static_cast<int64_t>(i);
    for (size_t i = 0; i < kAf32; ++i)
        (*af32)[i] = static_cast<float>(id + i) * 0.001f;
    for (size_t i = 0; i < kAf64; ++i)
        (*af64)[i] = static_cast<double>(id + i) * 0.0001;
}

void PopulateDoc(Doc* d, int64_t id, const std::string& big_str, const std::string& big_bytes,
                 const std::vector<int32_t>& a32, const std::vector<int64_t>& a64,
                 const std::vector<float>& af32, const std::vector<double>& af64) {
    d->put_int64(kFidId, id);
    d->put_int32(kFidBool, (id & 1) ? 1 : 0);
    d->put_int32(kFidI32, static_cast<int32_t>(id ^ 0x5a5a5a5a));
    d->put_int64(kFidI64, id * 1000003);
    d->put_float(kFidF32, static_cast<float>((id % 10000) * 0.25f));
    d->put_double(kFidF64, static_cast<double>(id) * 0.000015);
    d->put_string(kFidStr, big_str);
    d->put_string(kFidBytes, std::string_view(big_bytes.data(), big_bytes.size()));
    d->array_put_int32(kFidAi32, a32.data(), static_cast<uint32_t>(a32.size()));
    d->array_put_int64(kFidAi64, a64.data(), static_cast<uint32_t>(a64.size()));
    d->array_put_float(kFidAf32, af32.data(), static_cast<uint32_t>(af32.size()));
    d->array_put_double(kFidAf64, af64.data(), static_cast<uint32_t>(af64.size()));
}

int RunFill(const Config& cfg) {
    std::error_code ec;
    fs::create_directories(cfg.db_path, ec);
    if (ec) {
        std::cerr << "create_directories: " << ec.message() << "\n";
        return 1;
    }

    const fs::path idx_dir = fs::path(cfg.db_path) / cfg.index_name;

    if (cfg.recreate && fs::exists(idx_dir)) {
        fs::remove_all(idx_dir, ec);
        if (ec) {
            std::cerr << "remove_all: " << ec.message() << "\n";
            return 1;
        }
    }

    Schema schema;
    std::string err;
    if (!schema.LoadJson(kWideSchemaJson, &err)) {
        std::cerr << "Schema: " << err << "\n";
        return 1;
    }

    DB::ResetForTest();
    DB::Init(MakeDbOptions(cfg));

    const fs::path meta_path = idx_dir / "index.meta.json";
    const bool       meta_exists = fs::exists(meta_path);

    if (meta_exists) {
        DB::Instance().OpenIndex(cfg.index_name);
    } else {
        DB::Instance().CreateKVIndex(cfg.index_name, schema);
    }

    KVIndex* idx = DB::Instance().GetKVIndex(cfg.index_name);

    const uint64_t target = static_cast<uint64_t>(cfg.target_gb * 1024.0 * 1024.0 * 1024.0);

    std::string big_str, big_bytes;
    std::vector<int32_t>  a32;
    std::vector<int64_t>  a64;
    std::vector<float>    af32;
    std::vector<double>   af64;

    int64_t next_id = 1;
    if (idx->Size() > 0) {
        next_id = static_cast<int64_t>(idx->Size()) + 1;
    }

    std::cout << "Filling until used_bytes >= " << (target / (1024 * 1024)) << " MiB ...\n";

    constexpr int kBatch = 4;
    while (idx->alloc()->GetStats().used_bytes < target) {
        std::vector<Doc> batch_docs;
        batch_docs.reserve(kBatch);
        for (int b = 0; b < kBatch && idx->alloc()->GetStats().used_bytes < target; ++b) {
            FillLargeStrings(next_id, &big_str, &big_bytes, &a32, &a64, &af32, &af64);
            batch_docs.push_back(idx->NewDoc());
            PopulateDoc(&batch_docs.back(), next_id, big_str, big_bytes, a32, a64, af32, af64);
            ++next_id;
        }
        std::vector<Doc*> ptrs;
        ptrs.reserve(batch_docs.size());
        for (auto& d : batch_docs) ptrs.push_back(&d);
        idx->BatchPut(ptrs);
        idx->Publish();

        if ((next_id & 63) == 0) {
            auto st = idx->alloc()->GetStats();
            std::cout << "  docs=" << idx->Size()
                      << " used_MiB=" << (st.used_bytes / (1024 * 1024)) << "\n";
        }
    }

    idx->Publish();
    auto fin = idx->alloc()->GetStats();
    std::cout << "Done. Size=" << idx->Size()
              << " used_bytes=" << fin.used_bytes
              << " (" << (fin.used_bytes / (1024.0 * 1024.0)) << " MiB)\n";

    DB::Instance().CloseAll();
    DB::ResetForTest();
    return 0;
}

int RunBenchRead(const Config& cfg) {
    DB::ResetForTest();
    DB::Init(MakeDbOptions(cfg));
    DB::Instance().OpenIndex(cfg.index_name);
    KVIndex* idx = DB::Instance().GetKVIndex(cfg.index_name);

    const size_t n = idx->Size();
    if (n == 0) {
        std::cerr << "empty index\n";
        return 1;
    }

    Doc               out;
    const auto        t0 = std::chrono::steady_clock::now();
    const uint64_t    ops = cfg.ops;
    for (uint64_t i = 0; i < ops; ++i) {
        const int64_t pk = 1 + static_cast<int64_t>(i % n);
        (void)idx->Get(std::to_string(pk), &out);
    }
    const auto t1      = std::chrono::steady_clock::now();
    const double sec   = std::chrono::duration<double>(t1 - t0).count();
    std::cout << "bench-read: ops=" << ops << " sec=" << sec
              << " ops/s=" << (static_cast<double>(ops) / sec)
              << " ns/op=" << (1e9 * sec / static_cast<double>(ops)) << "\n";

    DB::Instance().CloseAll();
    DB::ResetForTest();
    return 0;
}

int RunBenchWrite(const Config& cfg) {
    Schema schema;
    std::string err;
    if (!schema.LoadJson(kWideSchemaJson, &err)) {
        std::cerr << "Schema: " << err << "\n";
        return 1;
    }

    DB::ResetForTest();
    DB::Init(MakeDbOptions(cfg));

    const fs::path idx_dir = fs::path(cfg.db_path) / (cfg.index_name + "_bench_wr");
    std::error_code ec;
    if (fs::exists(idx_dir)) fs::remove_all(idx_dir, ec);

    DB::Instance().CreateKVIndex(cfg.index_name + "_bench_wr", schema);
    KVIndex* idx = DB::Instance().GetKVIndex(cfg.index_name + "_bench_wr");

    std::string big_str, big_bytes;
    std::vector<int32_t> a32;
    std::vector<int64_t> a64;
    std::vector<float>   af32;
    std::vector<double>  af64;

    const auto     t0 = std::chrono::steady_clock::now();
    const uint64_t ops = cfg.ops;
    for (uint64_t i = 0; i < ops; ++i) {
        const int64_t id = static_cast<int64_t>(i + 1);
        FillBenchPayload(id, &big_str, &big_bytes, &a32, &a64, &af32, &af64);
        Doc d = idx->NewDoc();
        PopulateDoc(&d, id, big_str, big_bytes, a32, a64, af32, af64);
        idx->Put(&d);
        idx->Publish();
    }
    const auto   t1    = std::chrono::steady_clock::now();
    const double sec   = std::chrono::duration<double>(t1 - t0).count();
    std::cout << "bench-write (put+publish per doc): ops=" << ops << " sec=" << sec
              << " ops/s=" << (static_cast<double>(ops) / sec) << "\n";

    DB::Instance().CloseAll();
    DB::ResetForTest();
    fs::remove_all(idx_dir, ec);
    return 0;
}

int RunBenchSwmr(const Config& cfg) {
    DB::ResetForTest();
    DB::Init(MakeDbOptions(cfg));
    DB::Instance().OpenIndex(cfg.index_name);
    KVIndex* idx = DB::Instance().GetKVIndex(cfg.index_name);

    const size_t n = idx->Size();
    if (n == 0) {
        std::cerr << "empty index (run fill first)\n";
        return 1;
    }

    std::atomic<bool>      stop{false};
    std::atomic<int64_t>   write_id{static_cast<int64_t>(n) + 1};
    std::atomic<uint64_t>  read_ops{0};
    std::atomic<uint64_t>  write_ops{0};

    std::vector<std::thread> readers;
    readers.reserve(static_cast<size_t>(cfg.threads));
    for (int t = 0; t < cfg.threads; ++t) {
        readers.emplace_back([&, t]() {
            std::mt19937                           rng(static_cast<uint32_t>(42 + t));
            std::uniform_int_distribution<size_t> dist(0, n ? n - 1 : 0);
            Doc                                    out;
            while (!stop.load(std::memory_order_acquire)) {
                const int64_t pk = 1 + static_cast<int64_t>(dist(rng));
                if (idx->Get(std::to_string(pk), &out)) {
                    read_ops.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }

    std::thread writer([&]() {
        std::string              bs, bb;
        std::vector<int32_t>     a32;
        std::vector<int64_t>     a64;
        std::vector<float>       af32;
        std::vector<double>      af64;
        while (!stop.load(std::memory_order_acquire)) {
            const int64_t id = write_id.fetch_add(1, std::memory_order_acq_rel);
            FillBenchPayload(id, &bs, &bb, &a32, &a64, &af32, &af64);
            Doc d = idx->NewDoc();
            PopulateDoc(&d, id, bs, bb, a32, a64, af32, af64);
            idx->Put(&d);
            idx->Publish();
            write_ops.fetch_add(1, std::memory_order_relaxed);
        }
    });

    const auto t0 = std::chrono::steady_clock::now();
    std::this_thread::sleep_for(std::chrono::seconds(cfg.seconds));
    stop.store(true, std::memory_order_release);
    for (auto& th : readers) th.join();
    stop.store(true, std::memory_order_release);
    writer.join();

    const auto   t1  = std::chrono::steady_clock::now();
    const double sec = std::chrono::duration<double>(t1 - t0).count();
    std::cout << "bench-swmr: " << cfg.seconds << "s wall, readers=" << cfg.threads
              << " read_ops=" << read_ops.load() << " write_ops=" << write_ops.load()
              << " read_ops/s=" << (read_ops.load() / sec)
              << " write_ops/s=" << (write_ops.load() / sec) << "\n";

    DB::Instance().CloseAll();
    DB::ResetForTest();
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        Usage();
        return 1;
    }
    const char* cmd = argv[1];
    Config      cfg;

    if (std::strcmp(cmd, "fill") == 0) {
        if (!ParseArgs(argc, argv, "fill", &cfg)) {
            Usage();
            return 1;
        }
        return RunFill(cfg);
    }
    if (std::strcmp(cmd, "bench-read") == 0) {
        if (!ParseArgs(argc, argv, "bench-read", &cfg)) {
            Usage();
            return 1;
        }
        return RunBenchRead(cfg);
    }
    if (std::strcmp(cmd, "bench-write") == 0) {
        if (!ParseArgs(argc, argv, "bench-write", &cfg)) {
            Usage();
            return 1;
        }
        return RunBenchWrite(cfg);
    }
    if (std::strcmp(cmd, "bench-swmr") == 0) {
        if (!ParseArgs(argc, argv, "bench-swmr", &cfg)) {
            Usage();
            return 1;
        }
        return RunBenchSwmr(cfg);
    }

    Usage();
    return 1;
}
