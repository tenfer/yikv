// yikv_server_bench — brpc load generator for FlatBuffers Get (yikv.db.YikvDb / Get).
//
// Phases (wall / cumulative ms in JSON): keys_load, channel_ready, warmup, bench_wall;
// micro-phases summed across successful RPCs: encode_sum, rpc_sum, decode_sum, index_get_sum;
// per-success latency_ms: rpc / e2e / index_get (p50, p95, p99).

#include "yikv_server_generated.h"

#include "rpc/rpc_constants.h"

#include <arrow/api.h>
#include <arrow/io/api.h>
#include <arrow/scalar.h>
#include <parquet/arrow/reader.h>

#include <brpc/channel.h>
#include <brpc/controller.h>
#include <brpc/rpc_dump.h>
#include <brpc/serialized_request.h>
#include <brpc/serialized_response.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <utility>
#include <string>
#include <thread>
#include <vector>

namespace {

using clock = std::chrono::steady_clock;

double Ms(std::chrono::nanoseconds ns) {
    return std::chrono::duration<double, std::milli>(ns).count();
}

struct Flags {
    std::string  server              = "127.0.0.1:9000";
    // KV table name (FlatBuffers GetRequest.table_name); must match server registry.
    std::string  index;
    int          workers           = 8;
    int          warmup            = 32;
    double       duration_sec      = 0;
    uint64_t     requests          = 0;
    uint64_t     max_keys          = 200'000;
    std::string  keys_file;
    std::string  local_parquet;
    std::string  pk_column;
    uint64_t     max_latency_samples = 0;
};

void Usage() {
    std::cerr
        << "Usage: yikv_server_bench --server HOST:PORT --index TABLE [options]\n"
        << "  (FlatBuffers Get via brpc baidu_std + BaiduMasterService; payload SerializedRequest.)\n"
        << "  Required: --index NAME  (must match yikv-server table directory name, e.g. dsp_test3)\n"
        << "  One of:\n"
        << "    --keys_file PATH          (one pk per line; empty lines skipped)\n"
        << "    --local_parquet PATH --pk NAME   (optional --max_keys N, default 200000)\n"
        << "  Modes:\n"
        << "    --duration_sec N   OR  --requests N   (default --requests 50000 if neither)\n"
        << "  Optional: --workers N (default 8), --warmup N (default 32)\n"
        << "            --max_latency_samples N (per worker, cap for p50/p95/p99; 0 = unlimited)\n";
}

bool ParseFlags(int argc, char** argv, Flags* f) {
    for (int i = 1; i < argc; ++i) {
        if ((std::strcmp(argv[i], "--server") == 0 || std::strcmp(argv[i], "--grpc_target") == 0) &&
            i + 1 < argc) {
            f->server = argv[++i];
        } else if ((std::strcmp(argv[i], "--index") == 0 || std::strcmp(argv[i], "--table") == 0) &&
                   i + 1 < argc) {
            f->index = argv[++i];
        } else if (std::strcmp(argv[i], "--workers") == 0 && i + 1 < argc) {
            f->workers = std::max(1, std::atoi(argv[++i]));
        } else if (std::strcmp(argv[i], "--warmup") == 0 && i + 1 < argc) {
            f->warmup = std::max(0, std::atoi(argv[++i]));
        } else if (std::strcmp(argv[i], "--duration_sec") == 0 && i + 1 < argc) {
            f->duration_sec = std::strtod(argv[++i], nullptr);
        } else if (std::strcmp(argv[i], "--requests") == 0 && i + 1 < argc) {
            f->requests = std::strtoull(argv[++i], nullptr, 10);
        } else if (std::strcmp(argv[i], "--max_keys") == 0 && i + 1 < argc) {
            f->max_keys = std::strtoull(argv[++i], nullptr, 10);
        } else if (std::strcmp(argv[i], "--keys_file") == 0 && i + 1 < argc) {
            f->keys_file = argv[++i];
        } else if (std::strcmp(argv[i], "--local_parquet") == 0 && i + 1 < argc) {
            f->local_parquet = argv[++i];
        } else if (std::strcmp(argv[i], "--pk") == 0 && i + 1 < argc) {
            f->pk_column = argv[++i];
        } else if (std::strcmp(argv[i], "--max_latency_samples") == 0 && i + 1 < argc) {
            f->max_latency_samples = std::strtoull(argv[++i], nullptr, 10);
        } else {
            std::cerr << "Unknown arg: " << argv[i] << "\n";
            return false;
        }
    }
    if (f->index.empty()) {
        std::cerr << "--index TABLE is required (e.g. --index dsp_test3)\n";
        return false;
    }
    if (f->keys_file.empty() == f->local_parquet.empty()) {
        std::cerr << "Exactly one of --keys_file or --local_parquet is required.\n";
        return false;
    }
    if (!f->local_parquet.empty() && f->pk_column.empty()) {
        std::cerr << "--local_parquet requires --pk\n";
        return false;
    }
    if (f->duration_sec > 0 && f->requests > 0) {
        std::cerr << "Specify only one of --duration_sec or --requests\n";
        return false;
    }
    if (f->duration_sec <= 0 && f->requests == 0) f->requests = 50'000;
    return true;
}

bool ArrowCellIsNull(const arrow::Array& col, int64_t row) { return col.IsNull(row); }

// Decode one cell to a pk string via GetScalar so dictionary-encoded Parquet columns work and we
// avoid invalid static_casts on the physical array type.
arrow::Status AppendPkString(const arrow::Array& col, int64_t row, std::string* out) {
    if (ArrowCellIsNull(col, row)) return arrow::Status::OK();
    arrow::Result<std::shared_ptr<arrow::Scalar>> sc_res = col.GetScalar(row);
    if (!sc_res.ok()) return sc_res.status();
    const arrow::Scalar& sc = **sc_res;
    if (!sc.is_valid) return arrow::Status::OK();

    switch (sc.type->id()) {
        case arrow::Type::NA:
            return arrow::Status::OK();
        case arrow::Type::BOOL:
            *out = static_cast<const arrow::BooleanScalar&>(sc).value ? "1" : "0";
            return arrow::Status::OK();
        case arrow::Type::INT8:
            *out = std::to_string(static_cast<int>(static_cast<const arrow::Int8Scalar&>(sc).value));
            return arrow::Status::OK();
        case arrow::Type::INT16:
            *out = std::to_string(static_cast<const arrow::Int16Scalar&>(sc).value);
            return arrow::Status::OK();
        case arrow::Type::INT32:
            *out = std::to_string(static_cast<const arrow::Int32Scalar&>(sc).value);
            return arrow::Status::OK();
        case arrow::Type::INT64:
            *out = std::to_string(static_cast<const arrow::Int64Scalar&>(sc).value);
            return arrow::Status::OK();
        case arrow::Type::UINT8:
            *out = std::to_string(static_cast<const arrow::UInt8Scalar&>(sc).value);
            return arrow::Status::OK();
        case arrow::Type::UINT16:
            *out = std::to_string(static_cast<const arrow::UInt16Scalar&>(sc).value);
            return arrow::Status::OK();
        case arrow::Type::UINT32:
            *out = std::to_string(static_cast<const arrow::UInt32Scalar&>(sc).value);
            return arrow::Status::OK();
        case arrow::Type::UINT64:
            *out = std::to_string(static_cast<const arrow::UInt64Scalar&>(sc).value);
            return arrow::Status::OK();
        case arrow::Type::FLOAT:
            *out = std::to_string(static_cast<const arrow::FloatScalar&>(sc).value);
            return arrow::Status::OK();
        case arrow::Type::DOUBLE:
            *out = std::to_string(static_cast<const arrow::DoubleScalar&>(sc).value);
            return arrow::Status::OK();
        case arrow::Type::STRING:
        case arrow::Type::LARGE_STRING:
        case arrow::Type::BINARY:
        case arrow::Type::LARGE_BINARY: {
            const auto& bs = static_cast<const arrow::BaseBinaryScalar&>(sc);
            if (!bs.value) return arrow::Status::OK();
            out->assign(reinterpret_cast<const char*>(bs.value->data()), bs.value->size());
            return arrow::Status::OK();
        }
        default:
            return arrow::Status::Invalid("unsupported pk arrow type: ", sc.type->ToString());
    }
}

arrow::Status LoadKeysParquet(const std::string& path, const std::string& pk, uint64_t max_keys,
                              std::vector<std::string>* keys) {
    ARROW_ASSIGN_OR_RAISE(auto infile, arrow::io::ReadableFile::Open(path));
    ARROW_ASSIGN_OR_RAISE(auto reader, parquet::arrow::OpenFile(infile, arrow::default_memory_pool()));
    ARROW_ASSIGN_OR_RAISE(auto rb_reader, reader->GetRecordBatchReader());
    while (keys->size() < max_keys) {
        arrow::Result<std::shared_ptr<arrow::RecordBatch>> batch_res = rb_reader->Next();
        if (!batch_res.ok()) return batch_res.status();
        std::shared_ptr<arrow::RecordBatch> batch = *std::move(batch_res);
        if (!batch) break;
        auto col = batch->GetColumnByName(pk);
        if (!col) return arrow::Status::Invalid("no column: ", pk);
        const int64_t n = batch->num_rows();
        for (int64_t r = 0; r < n && keys->size() < max_keys; ++r) {
            std::string k;
            ARROW_RETURN_NOT_OK(AppendPkString(*col, r, &k));
            if (!k.empty()) keys->push_back(std::move(k));
        }
    }
    if (keys->empty()) return arrow::Status::Invalid("no keys loaded from parquet");
    return arrow::Status::OK();
}

void LoadKeysFile(const std::string& path, uint64_t max_keys, std::vector<std::string>* keys) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("cannot open keys_file: " + path);
    std::string line;
    while (std::getline(in, line) && keys->size() < max_keys) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
            line.pop_back();
        if (line.empty()) continue;
        if (line.rfind("oss://", 0) == 0) {
            std::cerr << "warning: skip OSS URI in keys file (use local paths only): " << line << "\n";
            continue;
        }
        keys->push_back(std::move(line));
    }
    if (keys->empty()) throw std::runtime_error("no keys in file");
}

struct BenchStats {
    std::atomic<uint64_t> ok{0};
    std::atomic<uint64_t> rpc_err{0};
    std::atomic<uint64_t> found{0};
    std::atomic<uint64_t> not_found{0};
    std::atomic<uint64_t> encode_ns{0};
    std::atomic<uint64_t> rpc_ns{0};
    std::atomic<uint64_t> decode_ns{0};
    std::atomic<uint64_t> index_get_ns{0};
};

double PercentileSortedNsMs(const std::vector<uint64_t>& sorted_ns, double p) {
    if (sorted_ns.empty()) return 0;
    const double           n = static_cast<double>(sorted_ns.size());
    size_t                 k = static_cast<size_t>(std::ceil(p * n));
    if (k == 0) k = 1;
    --k;
    if (k >= sorted_ns.size()) k = sorted_ns.size() - 1;
    return Ms(std::chrono::nanoseconds(sorted_ns[k]));
}

struct WorkerLatencySamples {
    std::vector<uint64_t> rpc_ns;
    std::vector<uint64_t> e2e_ns;
    std::vector<uint64_t> index_get_ns;
};

static void OneGetRpc(brpc::Channel* channel, const std::string& req_bytes, std::string* resp_out,
                      brpc::Controller* cntl) {
    // IOBuf::copy_to(string) returns 0 without touching *s when the body is empty; always clear
    // first so we never decode a stale FlatBuffer from a previous RPC.
    resp_out->clear();

    auto* sampled = new brpc::SampledRequest();
    sampled->meta.set_service_name(yikv_server::rpc::kServiceFullName);
    sampled->meta.set_method_name(yikv_server::rpc::kMethodGet);
    cntl->reset_sampled_request(sampled);

    brpc::SerializedRequest  sreq;
    brpc::SerializedResponse sres;
    sreq.serialized_data().append(req_bytes.data(), req_bytes.size());

    channel->CallMethod(nullptr, cntl, &sreq, &sres, nullptr);
    if (!cntl->Failed()) {
        sres.serialized_data().copy_to(resp_out);
    }
}

static std::string BuildGetRequestPayload(const std::string& pk, const std::string& table_name) {
    flatbuffers::FlatBufferBuilder fbb(256);
    auto                           pkoff = fbb.CreateString(pk);
    auto                           tnoff = fbb.CreateString(table_name);
    fbb.Finish(yikv::CreateGetRequest(fbb, pkoff, tnoff));
    return {reinterpret_cast<const char*>(fbb.GetBufferPointer()), fbb.GetSize()};
}

// FlatBuffers reads scalars at the buffer base; std::string::data() is not guaranteed
// alignof(uoffset_t)-aligned (SSO / allocator quirks), which faults on strict-alignment CPUs.
// Copy into a vector and use the first suitably aligned sub-span — no platform-specific free().
static const uint8_t* AlignFlatbufferInVector(const std::string& resp_str,
                                              std::vector<uint8_t>* storage) {
    storage->clear();
    const size_t n = resp_str.size();
    if (n < sizeof(flatbuffers::uoffset_t)) return nullptr;
    constexpr size_t kAlign = alignof(flatbuffers::uoffset_t);
    storage->resize(n + kAlign);
    uint8_t* const    base = storage->data();
    const uintptr_t   addr = reinterpret_cast<uintptr_t>(base);
    const size_t      skip = (kAlign - (addr % kAlign)) % kAlign;
    uint8_t* const    dst  = base + skip;
    std::memcpy(dst, resp_str.data(), n);
    return dst;
}
void WorkerLoop(const std::string& server, const std::vector<std::string>* keys,
                const std::string& table_name, int wid, int num_workers, BenchStats* st,
                WorkerLatencySamples* lat, uint64_t max_lat_samples, std::atomic<bool>* stop,
                uint64_t request_cap, bool use_duration, clock::time_point bench_deadline) {
    brpc::Channel        channel;
    brpc::ChannelOptions chopt;
    chopt.protocol = "baidu_std";
    if (channel.Init(server.c_str(), &chopt) != 0) {
        std::cerr << "worker " << wid << ": brpc::Channel::Init failed for " << server << "\n";
        stop->store(true, std::memory_order_release);
        return;
    }

    size_t       idx = static_cast<size_t>(wid);
    const size_t kn  = keys->size();

    while (!stop->load(std::memory_order_acquire)) {
        if (!use_duration) {
            uint64_t done = st->ok.load() + st->rpc_err.load();
            if (done >= request_cap) break;
        } else {
            if (clock::now() >= bench_deadline) break;
        }

        const std::string& pk = (*keys)[idx % kn];
        idx += static_cast<size_t>(num_workers);

        uint64_t enc_ns = 0;
        std::string req_bytes;
        {
            auto t0 = clock::now();
            req_bytes = BuildGetRequestPayload(pk, table_name);
            auto t1 = clock::now();
            enc_ns = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
            st->encode_ns.fetch_add(enc_ns, std::memory_order_relaxed);
        }

        brpc::Controller cntl;
        auto             t_rpc0 = clock::now();
        std::string       resp_str;
        OneGetRpc(&channel, req_bytes, &resp_str, &cntl);
        auto t_rpc1 = clock::now();
        uint64_t rpc1_ns = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(t_rpc1 - t_rpc0).count());
        st->rpc_ns.fetch_add(rpc1_ns, std::memory_order_relaxed);

        if (cntl.Failed()) {
            st->rpc_err.fetch_add(1, std::memory_order_relaxed);
            continue;
        }

        if (resp_str.size() < sizeof(flatbuffers::uoffset_t)) {
            st->rpc_err.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        std::vector<uint8_t> fb_storage;
        const uint8_t* fb = AlignFlatbufferInVector(resp_str, &fb_storage);
        if (!fb) {
            st->rpc_err.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        // Do not run full VerifyBuffer: wide GetResponse rows recurse deeply and can blow stack
        // or dominate CPU; server is trusted to return a valid buffer for Get.
        auto t_d0 = clock::now();
        const auto* gr = flatbuffers::GetRoot<yikv::GetResponse>(fb);
        if (gr->found())
            st->found.fetch_add(1, std::memory_order_relaxed);
        else
            st->not_found.fetch_add(1, std::memory_order_relaxed);
        uint64_t idx_get_ns = gr->index_get_ns();
        st->index_get_ns.fetch_add(idx_get_ns, std::memory_order_relaxed);
        auto     t_d1   = clock::now();
        uint64_t dec_ns = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(t_d1 - t_d0).count());
        st->decode_ns.fetch_add(dec_ns, std::memory_order_relaxed);

        if (lat != nullptr) {
            const bool cap_ok =
                max_lat_samples == 0 ||
                (lat->rpc_ns.size() < static_cast<size_t>(max_lat_samples) &&
                 lat->e2e_ns.size() < static_cast<size_t>(max_lat_samples) &&
                 lat->index_get_ns.size() < static_cast<size_t>(max_lat_samples));
            if (cap_ok) {
                lat->rpc_ns.push_back(rpc1_ns);
                lat->e2e_ns.push_back(enc_ns + rpc1_ns + dec_ns);
                lat->index_get_ns.push_back(idx_get_ns);
            }
        }

        st->ok.fetch_add(1, std::memory_order_relaxed);
    }
}

void PrintStartupParams(const Flags& fl) {
    std::cerr << "yikv_server_bench startup params:\n"
              << "  server=" << fl.server << "\n"
              << "  index=" << fl.index << "\n"
              << "  workers=" << fl.workers << "\n"
              << "  warmup=" << fl.warmup << "\n";
    if (fl.duration_sec > 0)
        std::cerr << "  duration_sec=" << fl.duration_sec << "\n";
    else
        std::cerr << "  requests=" << fl.requests << "\n";
    std::cerr << "  max_keys=" << fl.max_keys << "\n"
              << "  max_latency_samples=" << fl.max_latency_samples << "\n";
    if (!fl.keys_file.empty())
        std::cerr << "  keys_file=" << fl.keys_file << "\n";
    if (!fl.local_parquet.empty())
        std::cerr << "  local_parquet=" << fl.local_parquet << "\n  pk=" << fl.pk_column << "\n";
}

}  // namespace

int main(int argc, char** argv) {
    Flags fl;
    if (!ParseFlags(argc, argv, &fl)) {
        Usage();
        return 2;
    }
    PrintStartupParams(fl);

    std::vector<std::string> keys;
    auto                     t_keys0 = clock::now();
    try {
        if (!fl.keys_file.empty()) LoadKeysFile(fl.keys_file, fl.max_keys, &keys);
        else {
            arrow::Status ast = LoadKeysParquet(fl.local_parquet, fl.pk_column, fl.max_keys, &keys);
            if (!ast.ok()) {
                std::cerr << "parquet keys: " << ast.ToString() << "\n";
                return 1;
            }
        }
    } catch (const std::exception& e) {
        std::cerr << e.what() << "\n";
        return 1;
    }
    auto keys_load_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - t_keys0);

    auto t_chan0 = clock::now();
    brpc::Channel        channel;
    brpc::ChannelOptions chopt;
    chopt.protocol = "baidu_std";
    if (channel.Init(fl.server.c_str(), &chopt) != 0) {
        std::cerr << "Fail to init brpc channel to " << fl.server << "\n";
        return 1;
    }
    auto channel_ready_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - t_chan0);

    auto t_w0 = clock::now();
    for (int w = 0; w < fl.warmup; ++w) {
        const std::string& pk = keys[static_cast<size_t>(w) % keys.size()];
        std::string        req_bytes = BuildGetRequestPayload(pk, fl.index);
        brpc::Controller  cntl;
        std::string       resp;
        OneGetRpc(&channel, req_bytes, &resp, &cntl);
    }
    auto warmup_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - t_w0);

    BenchStats                        stats;
    std::vector<WorkerLatencySamples> wlat(static_cast<size_t>(fl.workers));
    std::atomic<bool>                 stop{false};
    const bool                        duration_mode = fl.duration_sec > 0;
    const uint64_t                    cap           = fl.requests;
    const clock::time_point bench_deadline =
        clock::now() + std::chrono::duration_cast<clock::duration>(
                           std::chrono::duration<double>(fl.duration_sec));

    auto t_b0 = clock::now();
    std::vector<std::thread> threads;
    threads.reserve(static_cast<size_t>(fl.workers));
    for (int w = 0; w < fl.workers; ++w) {
        threads.emplace_back([&, w] {
            WorkerLoop(fl.server, &keys, fl.index, w, fl.workers, &stats,
                       &wlat[static_cast<size_t>(w)], fl.max_latency_samples, &stop, cap,
                       duration_mode, bench_deadline);
        });
    }

    if (duration_mode) {
        std::this_thread::sleep_for(std::chrono::duration<double>(fl.duration_sec));
        stop.store(true, std::memory_order_release);
    } else {
        while (true) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            uint64_t d = stats.ok.load() + stats.rpc_err.load();
            if (d >= cap) {
                stop.store(true, std::memory_order_release);
                break;
            }
        }
    }
    for (auto& th : threads) th.join();
    auto bench_wall_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - t_b0);

    const uint64_t ok       = stats.ok.load();
    const double   wall_sec = Ms(bench_wall_ns) / 1000.0;
    const double   qps =
        (wall_sec > 0) ? static_cast<double>(ok + stats.rpc_err.load()) / wall_sec : 0;

    const uint64_t enc     = stats.encode_ns.load();
    const uint64_t rpc     = stats.rpc_ns.load();
    const uint64_t dec     = stats.decode_ns.load();
    const uint64_t idx_get = stats.index_get_ns.load();

    std::vector<uint64_t> all_rpc;
    std::vector<uint64_t> all_e2e;
    std::vector<uint64_t> all_index_get;
    size_t                merged_n = 0;
    for (auto& w : wlat) merged_n += w.rpc_ns.size();
    all_rpc.reserve(merged_n);
    all_e2e.reserve(merged_n);
    all_index_get.reserve(merged_n);
    for (auto& w : wlat) {
        all_rpc.insert(all_rpc.end(), w.rpc_ns.begin(), w.rpc_ns.end());
        all_e2e.insert(all_e2e.end(), w.e2e_ns.begin(), w.e2e_ns.end());
        all_index_get.insert(all_index_get.end(), w.index_get_ns.begin(), w.index_get_ns.end());
    }
    std::sort(all_rpc.begin(), all_rpc.end());
    std::sort(all_e2e.begin(), all_e2e.end());
    std::sort(all_index_get.begin(), all_index_get.end());
    const bool latency_truncated = ok > merged_n;

    std::ostringstream json;
    json << std::fixed << std::setprecision(3);
    json << "{\n"
         << "  \"server\": \"" << fl.server << "\",\n"
         << "  \"index\": \"" << fl.index << "\",\n"
         << "  \"protocol\": \"baidu_std\",\n"
         << "  \"service\": \"" << yikv_server::rpc::kServiceFullName << "\",\n"
         << "  \"method\": \"" << yikv_server::rpc::kMethodGet << "\",\n"
         << "  \"workers\": " << fl.workers << ",\n"
         << "  \"key_count\": " << keys.size() << ",\n"
         << "  \"requests_ok\": " << ok << ",\n"
         << "  \"requests_rpc_error\": " << stats.rpc_err.load() << ",\n"
         << "  \"found\": " << stats.found.load() << ",\n"
         << "  \"not_found\": " << stats.not_found.load() << ",\n"
         << "  \"qps\": " << qps << ",\n";
    if (merged_n == 0) {
        json << "  \"latency_ms\": null,\n";
    } else {
        json << "  \"latency_ms\": {\n"
             << "    \"sample_count\": " << merged_n << ",\n"
             << "    \"truncated\": " << (latency_truncated ? "true" : "false") << ",\n"
             << "    \"rpc\": {\n"
             << "      \"p50\": " << PercentileSortedNsMs(all_rpc, 0.50) << ",\n"
             << "      \"p95\": " << PercentileSortedNsMs(all_rpc, 0.95) << ",\n"
             << "      \"p99\": " << PercentileSortedNsMs(all_rpc, 0.99) << "\n"
             << "    },\n"
             << "    \"e2e\": {\n"
             << "      \"p50\": " << PercentileSortedNsMs(all_e2e, 0.50) << ",\n"
             << "      \"p95\": " << PercentileSortedNsMs(all_e2e, 0.95) << ",\n"
             << "      \"p99\": " << PercentileSortedNsMs(all_e2e, 0.99) << "\n"
             << "    },\n"
             << "    \"index_get\": {\n"
             << "      \"p50\": " << PercentileSortedNsMs(all_index_get, 0.50) << ",\n"
             << "      \"p95\": " << PercentileSortedNsMs(all_index_get, 0.95) << ",\n"
             << "      \"p99\": " << PercentileSortedNsMs(all_index_get, 0.99) << "\n"
             << "    }\n"
             << "  },\n";
    }
    json << "  \"phases_ms\": {\n"
         << "    \"keys_load\": " << Ms(keys_load_ns) << ",\n"
         << "    \"channel_ready\": " << Ms(channel_ready_ns) << ",\n"
         << "    \"warmup\": " << Ms(warmup_ns) << ",\n"
         << "    \"bench_wall\": " << Ms(bench_wall_ns) << ",\n"
         << "    \"encode_sum\": " << Ms(std::chrono::nanoseconds(enc)) << ",\n"
         << "    \"rpc_sum\": " << Ms(std::chrono::nanoseconds(rpc)) << ",\n"
         << "    \"decode_sum\": " << Ms(std::chrono::nanoseconds(dec)) << ",\n"
         << "    \"index_get_sum\": " << Ms(std::chrono::nanoseconds(idx_get)) << "\n"
         << "  },\n"
         << "  \"avg_per_ok_rpc_ms\": ";
    if (ok == 0) {
        json << "null\n";
    } else {
        json << "{\n"
             << "    \"encode\": " << (Ms(std::chrono::nanoseconds(enc)) / static_cast<double>(ok))
             << ",\n"
             << "    \"rpc\": " << (Ms(std::chrono::nanoseconds(rpc)) / static_cast<double>(ok))
             << ",\n"
             << "    \"decode\": " << (Ms(std::chrono::nanoseconds(dec)) / static_cast<double>(ok))
             << ",\n"
             << "    \"index_get\": "
             << (Ms(std::chrono::nanoseconds(idx_get)) / static_cast<double>(ok)) << "\n"
             << "  }\n";
    }
    json << "}\n";
    std::cout << json.str();
    return 0;
}
