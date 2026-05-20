// yikv_import_pipeline — Phase A: indexer module (multi IO Source workers + single KV write thread).

#include "indexer/publisher/io_publisher.h"
#include "indexer/source/file/file_source.h"
#include "indexer/source/file/cloud_filesystem.h"
#include "indexer/source/sql/mysql_wire_source.h"
#include "indexer/worker/kv_write_worker.h"
#include "kafka/kafka_import_catchup.h"
#include "server_config.h"
#include "table_config.h"
#include "db/db.h"
#include "index/kv_index.h"

#include <arrow/filesystem/filesystem.h>
#include <arrow/io/api.h>
#include <arrow/util/config.h>
#ifdef ARROW_S3
#include <arrow/filesystem/s3fs.h>
#endif
#include <parquet/arrow/reader.h>

#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

using yikv::alloc::AllocatorMode;
using yikv::db::DB;
using yikv::db::DBOptions;
using yikv::schema::Schema;

namespace {

static std::string WallTimestamp() {
    const auto        now = std::chrono::system_clock::now();
    const std::time_t t   = std::chrono::system_clock::to_time_t(now);
    struct tm         tm_buf {};
    ::localtime_r(&t, &tm_buf);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm_buf);
    return buf;
}

static bool EndsWithExtensionIgnoreCase(const fs::path& p, std::string_view ext) {
    std::string e = p.extension().string();
    for (char& ch : e) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return e == ext;
}

static bool IsImportPipelineDataFile(const fs::path& p) {
    return EndsWithExtensionIgnoreCase(p, ".parquet") || EndsWithExtensionIgnoreCase(p, ".csv");
}

static bool CollectDataFilesUnderDir(const fs::path& dir, std::vector<std::string>* out, std::string* err) {
    std::error_code ec;
    if (!fs::exists(dir, ec) || ec) {
        *err = "path does not exist: " + dir.string();
        return false;
    }
    if (!fs::is_directory(dir, ec) || ec) {
        *err = "not a directory: " + dir.string();
        return false;
    }
    std::vector<fs::path> found;
    try {
        const auto opt = fs::directory_options::skip_permission_denied;
        for (const auto& ent : fs::recursive_directory_iterator(dir, opt)) {
            std::error_code fst;
            if (!ent.is_regular_file(fst) || fst) continue;
            if (!IsImportPipelineDataFile(ent.path())) continue;
            found.push_back(ent.path());
        }
    } catch (const fs::filesystem_error& e) {
        *err = e.what();
        return false;
    }
    if (found.empty()) {
        *err = "no .parquet or .csv files under " + dir.string();
        return false;
    }
    std::sort(found.begin(), found.end());
    for (const auto& p : found) out->push_back(p.string());
    return true;
}

std::string ReadFile(const std::string& path) {
    std::ifstream in(path, std::ios::in | std::ios::binary);
    if (!in) throw std::runtime_error("cannot read file: " + path);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

static uint64_t EstimateCsvDataRowsByLineCount(const std::string& path) {
    std::ifstream in(path, std::ios::in | std::ios::binary);
    if (!in) return 0;
    uint64_t lines = 0;
    std::string  line;
    while (std::getline(in, line)) ++lines;
    return lines > 0 ? lines - 1 : 0;
}

static bool PathViewEndsWithIgnoreCase(std::string_view path, std::string_view suf) {
    if (path.size() < suf.size()) return false;
    for (size_t i = 0; i < suf.size(); ++i) {
        const char c = static_cast<char>(std::tolower(static_cast<unsigned char>(path[path.size() - suf.size() + i])));
        if (c != suf[i]) return false;
    }
    return true;
}

static uint64_t EstimateTotalRows(const std::vector<std::string>& files, const yikv::indexer::CloudFileSystems& cfs) {
    uint64_t total = 0;
    for (const auto& path : files) {
        if (yikv::indexer::IsCloudUri(path)) {
            std::string scheme;
            std::string bucket;
            std::string key;
            if (!yikv::indexer::ParseCloudUri(path, &scheme, &bucket, &key)) continue;
            arrow::fs::FileSystem* fs = cfs.FsForScheme(scheme);
            if (!fs) continue;
            const std::string fs_path = bucket + "/" + key;
            if (PathViewEndsWithIgnoreCase(path, ".csv")) continue;
            if (!PathViewEndsWithIgnoreCase(path, ".parquet")) continue;
            auto infile_res = fs->OpenInputFile(fs_path);
            if (!infile_res.ok()) continue;
            auto reader_res = parquet::arrow::OpenFile(*infile_res, arrow::default_memory_pool());
            if (!reader_res.ok()) continue;
            auto* pr = (*reader_res)->parquet_reader();
            if (pr && pr->metadata()) total += static_cast<uint64_t>(pr->metadata()->num_rows());
            continue;
        }

        const fs::path p(path);
        if (EndsWithExtensionIgnoreCase(p, ".csv")) {
            total += EstimateCsvDataRowsByLineCount(path);
            continue;
        }
        if (!EndsWithExtensionIgnoreCase(p, ".parquet")) continue;
        auto infile_res = arrow::io::ReadableFile::Open(path);
        if (!infile_res.ok()) continue;
        auto reader_res = parquet::arrow::OpenFile(*infile_res, arrow::default_memory_pool());
        if (!reader_res.ok()) continue;
        auto* pr = (*reader_res)->parquet_reader();
        if (pr && pr->metadata()) total += static_cast<uint64_t>(pr->metadata()->num_rows());
    }
    return total;
}

static uint32_t BucketBitsForRows(uint64_t n_rows) {
    if (n_rows == 0) return 15;
    uint32_t bits    = 1;
    uint64_t buckets = 2;
    while (buckets < n_rows) {
        buckets <<= 1;
        ++bits;
    }
    return (bits > 28) ? 28u : bits;
}

void Usage() {
    std::cerr
        << "yikv_import_pipeline — parallel Parquet/CSV read/parse + single-thread KV write (Phase A).\n"
        << "Usage:\n"
        << "  yikv_import_pipeline --config config.json --index NAME \\\n"
        << "         (--input FILE)+ | --input_list PATH | --input_dir PATH \\\n"
        << "         [--schema_json PATH] [--create_if_missing] [--recreate] [--no_arena_lock] \\\n"
        << "         [--import_io_workers N] [--import_queue_batches M]\n"
        << "\n"
        << "MySQL-compatible (--mysql_query): stream SELECT into the same Arrow→KV path (StarRocks, MySQL, …).\n"
        << "  Requires --mysql_query or --mysql_query_file and connection (--mysql_* or MYSQL_* / YIKV_MYSQL_* env).\n"
        << "  optional --mysql_password, --mysql_port, --mysql_batch_rows, --mysql_init_sql FILE, --sql_est_rows.\n"
        << "  Omit file --input* when using --mysql_query. Use --import_io_workers 1 unless you know the source is thread-safe.\n"
        << "\n"
        << "Local: .parquet/.csv paths or --input_dir (recursive). CSV: header row, no array fields.\n"
        << "Remote: oss:// s3:// cos:// obs:// gs:// — object or prefix/ (trailing / lists .parquet/.csv).\n"
        << "  oss://: OSS_ENDPOINT, OSS_ACCESS_KEY_ID, OSS_ACCESS_KEY_SECRET; optional OSS_REGION.\n"
        << "  s3://:  AWS default credential chain.\n"
        << "  cos://: COS_SECRET_ID, COS_SECRET_KEY; COS_ENDPOINT or COS_REGION (cos.<region>.myqcloud.com).\n"
        << "  obs://: OBS_ENDPOINT, OBS_ACCESS_KEY_ID, OBS_SECRET_ACCESS_KEY; optional OBS_REGION.\n"
        << "  gs://:  Google Application Default Credentials.\n"
        << "  --import_io_workers    Parallel reader/parser threads (default 4).\n"
        << "  --import_queue_batches Max RecordBatches in flight on the bounded queue (default 32).\n"
        << "  Kafka catch-up (after bulk import; needs per-table table.json \"kafka\" block):\n"
        << "    --kafka_catchup                     Consume incrementals and write kafka.offset + kafka_meta.json.\n"
        << "    --kafka_offline_watermark_sec SEC   Epoch seconds for offsets_for_times (required with --kafka_catchup).\n"
        << "    --kafka_rewind_minutes N            Rewrite start time as SEC - N*60 before offset lookup (default 0).\n"
        << "    --kafka_catchup_wall_sec N          Max wall seconds for catch-up loop (0 = use default 7200).\n"
        << "    Brokers: table.json kafka.brokers, else config.json kafka.default_brokers.\n"
        << "  MySQL env (used when CLI omits; CLI wins): MYSQL_HOST, MYSQL_USER, MYSQL_PASSWORD or MYSQL_PWD,\n"
        << "    MYSQL_DATABASE, MYSQL_TCP_PORT or MYSQL_PORT; same with YIKV_MYSQL_* prefix.\n";
}

struct Flags {
    std::string              config_path;
    std::string              db_path;
    std::string              index_name;
    std::string              schema_json_path;
    bool                     create_if_missing = false;
    bool                     recreate          = false;
    bool                     no_arena_lock     = false;
    bool                     exclusive_arena_lock_from_cfg = true;
    uint64_t                 arena_seg_gb = 0;
    uint64_t                 arena_max_gb = 0;
    std::vector<std::string> input_files;
    unsigned                 import_io_workers     = 4;
    size_t                   import_queue_batches = 32;
    std::string              mysql_host;
    std::uint16_t            mysql_port          = 3306;
    std::string              mysql_user;
    std::string              mysql_password;
    std::string              mysql_database;
    std::string              mysql_query;
    std::string              mysql_query_file;
    std::string              mysql_init_sql_file;
    std::size_t              mysql_batch_rows = 4096;
    std::uint64_t            sql_est_rows     = 0;
    bool                     mysql_port_from_cli = false;
    bool                     kafka_catchup = false;
    bool                     kafka_watermark_set           = false;
    int64_t                  kafka_offline_watermark_sec = 0;
    uint32_t                 kafka_rewind_minutes        = 0;
    int                      kafka_catchup_wall_sec      = 0;
    std::string              kafka_default_brokers;
};

static std::vector<std::string> ReadInitSqlLines(const std::string& path) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("cannot read --mysql_init_sql file: " + path);
    std::vector<std::string> lines;
    std::string              line;
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
            line.pop_back();
        if (!line.empty()) lines.push_back(std::move(line));
    }
    return lines;
}

static const char* EnvNonEmpty(const char* key) {
    const char* v = std::getenv(key);
    if (v && v[0] != '\0') return v;
    return nullptr;
}

/// Fill MySQL connection fields from environment when CLI left them unset. CLI always wins.
static void FillMysqlFlagsFromEnv(Flags* f) {
    if (f->mysql_host.empty()) {
        if (const char* v = EnvNonEmpty("MYSQL_HOST")) f->mysql_host = v;
        else if (const char* v = EnvNonEmpty("YIKV_MYSQL_HOST")) f->mysql_host = v;
    }
    if (f->mysql_user.empty()) {
        if (const char* v = EnvNonEmpty("MYSQL_USER")) f->mysql_user = v;
        else if (const char* v = EnvNonEmpty("YIKV_MYSQL_USER")) f->mysql_user = v;
    }
    if (f->mysql_password.empty()) {
        if (const char* v = EnvNonEmpty("MYSQL_PASSWORD")) f->mysql_password = v;
        else if (const char* v = EnvNonEmpty("YIKV_MYSQL_PASSWORD")) f->mysql_password = v;
        else if (const char* v = std::getenv("MYSQL_PWD"))
            f->mysql_password = v;  // legacy; may be empty string
    }
    if (f->mysql_database.empty()) {
        if (const char* v = EnvNonEmpty("MYSQL_DATABASE")) f->mysql_database = v;
        else if (const char* v = EnvNonEmpty("YIKV_MYSQL_DATABASE")) f->mysql_database = v;
    }
    if (!f->mysql_port_from_cli) {
        const char* p = EnvNonEmpty("MYSQL_TCP_PORT");
        if (!p) p = EnvNonEmpty("MYSQL_PORT");
        if (!p) p = EnvNonEmpty("YIKV_MYSQL_PORT");
        if (p) {
            const unsigned long n = std::strtoul(p, nullptr, 10);
            if (n > 0 && n <= 65535u) f->mysql_port = static_cast<std::uint16_t>(n);
        }
    }
}

bool ParseFlags(int argc, char** argv, Flags* f) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
            f->config_path = argv[++i];
        } else if (std::strcmp(argv[i], "--index") == 0 && i + 1 < argc) {
            f->index_name = argv[++i];
        } else if (std::strcmp(argv[i], "--schema_json") == 0 && i + 1 < argc) {
            f->schema_json_path = argv[++i];
        } else if (std::strcmp(argv[i], "--input") == 0 && i + 1 < argc) {
            f->input_files.push_back(argv[++i]);
        } else if (std::strcmp(argv[i], "--input_list") == 0 && i + 1 < argc) {
            std::ifstream in(argv[++i]);
            if (!in) {
                std::cerr << "cannot open --input_list\n";
                return false;
            }
            std::string line;
            while (std::getline(in, line)) {
                if (line.empty()) continue;
                while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
                    line.pop_back();
                if (!line.empty()) f->input_files.push_back(line);
            }
        } else if (std::strcmp(argv[i], "--input_dir") == 0 && i + 1 < argc) {
            std::string err;
            if (!CollectDataFilesUnderDir(fs::path(argv[++i]), &f->input_files, &err)) {
                std::cerr << "--input_dir: " << err << "\n";
                return false;
            }
        } else if (std::strcmp(argv[i], "--create_if_missing") == 0) {
            f->create_if_missing = true;
        } else if (std::strcmp(argv[i], "--recreate") == 0) {
            f->recreate = true;
        } else if (std::strcmp(argv[i], "--no_arena_lock") == 0) {
            f->no_arena_lock = true;
        } else if (std::strcmp(argv[i], "--import_io_workers") == 0 && i + 1 < argc) {
            f->import_io_workers = static_cast<unsigned>(std::strtoul(argv[++i], nullptr, 10));
            if (f->import_io_workers < 1) f->import_io_workers = 1;
        } else if (std::strcmp(argv[i], "--import_queue_batches") == 0 && i + 1 < argc) {
            f->import_queue_batches = std::strtoull(argv[++i], nullptr, 10);
            if (f->import_queue_batches < 1) f->import_queue_batches = 1;
        } else if (std::strcmp(argv[i], "--mysql_host") == 0 && i + 1 < argc) {
            f->mysql_host = argv[++i];
        } else if (std::strcmp(argv[i], "--mysql_user") == 0 && i + 1 < argc) {
            f->mysql_user = argv[++i];
        } else if (std::strcmp(argv[i], "--mysql_password") == 0 && i + 1 < argc) {
            f->mysql_password = argv[++i];
        } else if (std::strcmp(argv[i], "--mysql_database") == 0 && i + 1 < argc) {
            f->mysql_database = argv[++i];
        } else if (std::strcmp(argv[i], "--mysql_query_file") == 0 && i + 1 < argc) {
            f->mysql_query_file = argv[++i];
        } else if (std::strcmp(argv[i], "--mysql_query") == 0 && i + 1 < argc) {
            f->mysql_query = argv[++i];
        } else if (std::strcmp(argv[i], "--mysql_port") == 0 && i + 1 < argc) {
            f->mysql_port_from_cli = true;
            const unsigned long p = std::strtoul(argv[++i], nullptr, 10);
            f->mysql_port = p > 65535 ? 3306 : static_cast<std::uint16_t>(p);
        } else if (std::strcmp(argv[i], "--mysql_batch_rows") == 0 && i + 1 < argc) {
            f->mysql_batch_rows = std::strtoull(argv[++i], nullptr, 10);
            if (f->mysql_batch_rows < 1) f->mysql_batch_rows = 1;
        } else if (std::strcmp(argv[i], "--mysql_init_sql") == 0 && i + 1 < argc) {
            f->mysql_init_sql_file = argv[++i];
        } else if (std::strcmp(argv[i], "--sql_est_rows") == 0 && i + 1 < argc) {
            f->sql_est_rows = std::strtoull(argv[++i], nullptr, 10);
        } else if (std::strcmp(argv[i], "--kafka_catchup") == 0) {
            f->kafka_catchup = true;
        } else if (std::strcmp(argv[i], "--kafka_offline_watermark_sec") == 0 && i + 1 < argc) {
            f->kafka_watermark_set = true;
            f->kafka_offline_watermark_sec =
                static_cast<int64_t>(std::strtoll(argv[++i], nullptr, 10));
        } else if (std::strcmp(argv[i], "--kafka_rewind_minutes") == 0 && i + 1 < argc) {
            f->kafka_rewind_minutes =
                static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
        } else if (std::strcmp(argv[i], "--kafka_catchup_wall_sec") == 0 && i + 1 < argc) {
            f->kafka_catchup_wall_sec = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "-h") == 0 || std::strcmp(argv[i], "--help") == 0) {
            Usage();
            return false;
        } else {
            std::cerr << "Unknown arg: " << argv[i] << "\n";
            return false;
        }
    }
    if (f->config_path.empty()) {
        std::cerr << "Required: --config FILE\n";
        return false;
    }
    if (f->index_name.empty()) {
        std::cerr << "Required: --index\n";
        return false;
    }
    const bool mysql_mode = !f->mysql_query.empty() || !f->mysql_query_file.empty();
    if (!mysql_mode && f->input_files.empty()) {
        std::cerr << "Required: file inputs (--input | --input_list | --input_dir) or MySQL mode (--mysql_query / --mysql_query_file).\n";
        return false;
    }
    if (mysql_mode) {
        if (!f->mysql_query.empty() && !f->mysql_query_file.empty()) {
            std::cerr << "Use only one of --mysql_query or --mysql_query_file.\n";
            return false;
        }
        if (!f->mysql_query_file.empty()) {
            try {
                f->mysql_query = ReadFile(f->mysql_query_file);
            } catch (const std::exception& e) {
                std::cerr << e.what() << "\n";
                return false;
            }
            if (f->mysql_query.empty()) {
                std::cerr << "--mysql_query_file is empty.\n";
                return false;
            }
        }
        if (f->mysql_query.empty()) {
            std::cerr << "MySQL mode requires --mysql_query or --mysql_query_file.\n";
            return false;
        }
        FillMysqlFlagsFromEnv(f);
        if (f->mysql_host.empty() || f->mysql_user.empty() || f->mysql_database.empty()) {
            std::cerr << "MySQL mode requires --mysql_host, --mysql_user, and --mysql_database "
                         "(or MYSQL_HOST / MYSQL_USER / MYSQL_DATABASE env, or YIKV_MYSQL_*).\n";
            return false;
        }
    }
    if (f->create_if_missing && f->schema_json_path.empty()) {
        std::cerr << "--create_if_missing requires --schema_json\n";
        return false;
    }
    if (f->kafka_catchup && !f->kafka_watermark_set) {
        std::cerr << "--kafka_catchup requires --kafka_offline_watermark_sec SEC (epoch seconds).\n";
        return false;
    }
    return true;
}

bool ApplyConfigFile(Flags* f) {
    try {
        const yikv_server::ServerConfig cfg  = yikv_server::LoadServerConfig(f->config_path);
        f->db_path                       = cfg.db_path;
        f->arena_seg_gb                  = cfg.arena_seg_gb;
        f->arena_max_gb                  = cfg.arena_max_gb;
        f->exclusive_arena_lock_from_cfg = cfg.exclusive_arena_lock;
        f->kafka_default_brokers         = cfg.kafka_default_brokers;
    } catch (const std::exception& e) {
        std::cerr << "--config: " << e.what() << "\n";
        return false;
    }
    std::cerr << WallTimestamp() << " import_pipeline using --config " << f->config_path
              << " db_path=" << f->db_path << " import_io_workers=" << f->import_io_workers
              << " import_queue_batches=" << f->import_queue_batches << "\n";
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    Flags fl;
    if (!ParseFlags(argc, argv, &fl)) return 2;
    if (!ApplyConfigFile(&fl)) return 2;

#ifdef ARROW_S3
    struct S3Shutdown {
        bool on = false;
        void arm() { on = true; }
        ~S3Shutdown() {
            if (!on) return;
            const auto st = arrow::fs::EnsureS3Finalized();
            if (!st.ok()) {
                std::cerr << WallTimestamp() << " EnsureS3Finalized: " << st.ToString() << "\n";
            }
        }
    } s3_shutdown;
#else
    struct S3Shutdown {
        void arm() {}
    };
    [[maybe_unused]] S3Shutdown s3_shutdown;
#endif

    yikv::indexer::CloudFileSystems cloud;
    bool                         need_oss = false;
    bool                         need_s3  = false;
    bool                         need_cos = false;
    bool                         need_obs = false;
    bool                         need_gcs = false;
    for (const auto& p : fl.input_files) {
        if (!yikv::indexer::IsCloudUri(p)) continue;
        std::string sch;
        std::string b;
        std::string k;
        if (!yikv::indexer::ParseCloudUri(p, &sch, &b, &k)) continue;
        if (sch == "oss")
            need_oss = true;
        else if (sch == "s3")
            need_s3 = true;
        else if (sch == "cos")
            need_cos = true;
        else if (sch == "obs")
            need_obs = true;
        else if (sch == "gs")
            need_gcs = true;
    }
    const bool need_s3_api = need_oss || need_s3 || need_cos || need_obs;
#ifdef ARROW_S3
    if (need_s3_api) {
        if (const auto st = arrow::fs::EnsureS3Initialized(); !st.ok()) {
            std::cerr << "EnsureS3Initialized: " << st.ToString() << "\n";
            return 2;
        }
        s3_shutdown.arm();
    }
#else
    if (need_s3_api) {
        std::cerr << "This build of Apache Arrow does not include S3 support (ARROW_S3). "
                       "oss://, s3://, cos://, and obs:// inputs require an Arrow install built with S3.\n";
        return 2;
    }
#endif
#ifndef ARROW_GCS
    if (need_gcs) {
        std::cerr << "This build of Apache Arrow does not include GCS support (ARROW_GCS). "
                     "gs:// inputs require an Arrow install built with GCS.\n";
        return 2;
    }
#endif
    if (need_oss) {
        auto fs_res = yikv::indexer::MakeOssFileSystem();
        if (!fs_res.ok()) {
            std::cerr << fs_res.status().ToString() << "\n";
            return 2;
        }
        cloud.oss = *std::move(fs_res);
    }
    if (need_s3) {
        auto fs_res = yikv::indexer::MakeAwsS3FileSystem();
        if (!fs_res.ok()) {
            std::cerr << fs_res.status().ToString() << "\n";
            return 2;
        }
        cloud.s3 = *std::move(fs_res);
    }
    if (need_cos) {
        auto fs_res = yikv::indexer::MakeTencentCosFileSystem();
        if (!fs_res.ok()) {
            std::cerr << fs_res.status().ToString() << "\n";
            return 2;
        }
        cloud.cos = *std::move(fs_res);
    }
    if (need_obs) {
        auto fs_res = yikv::indexer::MakeHuaweiObsFileSystem();
        if (!fs_res.ok()) {
            std::cerr << fs_res.status().ToString() << "\n";
            return 2;
        }
        cloud.obs = *std::move(fs_res);
    }
    if (need_gcs) {
        auto fs_res = yikv::indexer::MakeGcsFileSystem();
        if (!fs_res.ok()) {
            std::cerr << fs_res.status().ToString() << "\n";
            return 2;
        }
        cloud.gcs = *std::move(fs_res);
    }
    if (need_oss || need_s3 || need_cos || need_obs || need_gcs) {
        auto ex = yikv::indexer::ExpandCloudInputs(cloud, &fl.input_files);
        if (!ex.ok()) {
            std::cerr << ex.ToString() << "\n";
            return 2;
        }
    }

    DBOptions opt;
    opt.db_path              = fl.db_path;
    opt.exclusive_arena_lock = fl.exclusive_arena_lock_from_cfg && !fl.no_arena_lock;
    opt.alloc_defaults.mode             = AllocatorMode::SingleWriter;
    opt.alloc_defaults.reclaim_delay_ns = 0;
    const uint64_t seg_b                = fl.arena_seg_gb * 1024ULL * 1024ULL * 1024ULL;
    opt.alloc_defaults.arena_size       = seg_b;
    opt.alloc_defaults.segment_size       = seg_b;
    opt.alloc_defaults.max_arena_size     = fl.arena_max_gb * 1024ULL * 1024ULL * 1024ULL;

    fs::path idx_dir = fs::path(fl.db_path) / fl.index_name;
    bool     exists  = fs::is_directory(idx_dir);

    if (fl.recreate && exists) {
        fs::remove_all(idx_dir);
        exists = false;
    }

    const uint64_t est_rows =
        fl.mysql_query.empty() ? EstimateTotalRows(fl.input_files, cloud) : fl.sql_est_rows;
    const uint32_t  bucket_bits = BucketBitsForRows(est_rows);
    std::cerr << WallTimestamp() << " import_pipeline_init estimated_rows=" << est_rows << " bucket_bits=" << bucket_bits
              << "\n";

    try {
        yikv::db::DB::Init(std::move(opt));
        if (!exists && fl.create_if_missing) {
            Schema      sch;
            std::string err;
            if (!sch.LoadJson(ReadFile(fl.schema_json_path), &err)) {
                std::cerr << "schema: " << err << "\n";
                return 1;
            }
            DB::Instance().CreateKVIndex(fl.index_name, sch, bucket_bits);
            const fs::path schema_out = fs::path(fl.db_path) / fl.index_name / "schema.json";
            try {
                std::ofstream sf(schema_out);
                sf << sch.ToJson();
                std::cerr << WallTimestamp() << " wrote " << schema_out << "\n";
            } catch (const std::exception& e) {
                std::cerr << WallTimestamp() << " WARNING: could not write schema.json: " << e.what() << "\n";
            }
        } else {
            DB::Instance().OpenIndex(fl.index_name);
        }
    } catch (const std::exception& e) {
        std::cerr << e.what() << "\n";
        return 1;
    }

    yikv::index::KVIndex* idx = DB::Instance().GetKVIndex(fl.index_name);
    idx->EnableBulkMode();
    const Schema* sch     = idx->schema();
    const size_t  n_files = fl.mysql_query.empty() ? fl.input_files.size() : 0;

    yikv::indexer::BoundedParsedBatchQueue queue(fl.import_queue_batches);
    yikv::indexer::ErrorState              err_st;
    std::unique_ptr<yikv::indexer::Source> source;
    if (!fl.mysql_query.empty()) {
        yikv::indexer::MysqlWireConfig cfg;
        cfg.host        = fl.mysql_host;
        cfg.port        = fl.mysql_port;
        cfg.user        = fl.mysql_user;
        cfg.password    = fl.mysql_password;
        cfg.database    = fl.mysql_database;
        cfg.query       = fl.mysql_query;
        cfg.batch_rows  = fl.mysql_batch_rows;
        try {
            if (!fl.mysql_init_sql_file.empty())
                cfg.init_sql = ReadInitSqlLines(fl.mysql_init_sql_file);
        } catch (const std::exception& e) {
            std::cerr << e.what() << "\n";
            DB::Instance().CloseAll();
            return 1;
        }
        source = std::make_unique<yikv::indexer::MysqlWireSource>(std::move(cfg), sch);
    } else {
        source = std::make_unique<yikv::indexer::FileSource>(std::move(fl.input_files), sch, std::move(cloud));
    }

    const auto t0              = std::chrono::steady_clock::now();
    uint64_t   total_rows     = 0;
    uint64_t   skipped_pk_null = 0;
    yikv::indexer::ImportProgress prog{};
    prog.t_start    = t0;
    prog.t_last_log = t0;

    std::thread writer([&] {
        yikv::indexer::RunKvWriteLoop(idx, sch, queue, err_st, &total_rows, &skipped_pk_null, &prog);
    });

    yikv::indexer::IoPublisher::Run(*source, fl.import_io_workers, queue, err_st);

    if (!err_st.failed.load()) queue.PushEos();
    else queue.Abort();

    writer.join();

    const auto   t1  = std::chrono::steady_clock::now();
    const double sec = std::chrono::duration<double>(t1 - t0).count();
    const bool   ok  = !err_st.failed.load();

    if (err_st.failed.load()) {
        std::lock_guard<std::mutex> lk(err_st.mu);
        std::cerr << WallTimestamp() << " import_pipeline FAILED: " << err_st.message << "\n";
    } else {
        std::cerr << WallTimestamp() << " {\"files\":" << n_files << ",\"mysql\":"
                  << (fl.mysql_query.empty() ? "false" : "true") << ",\"rows\":" << total_rows
                  << ",\"skipped_pk_null\":" << skipped_pk_null << ",\"wall_sec\":" << sec
                  << ",\"rows_per_sec\":" << (sec > 0 ? static_cast<double>(total_rows) / sec : 0.0) << "}\n";
    }

    if (!ok) {
        DB::Instance().CloseAll();
        return 1;
    }

    try {
        DB::Instance().PersistIndexMeta(fl.index_name);
    } catch (const std::exception& e) {
        std::cerr << WallTimestamp() << " PersistIndexMeta: " << e.what() << "\n";
        DB::Instance().CloseAll();
        return 1;
    }

    if (fl.kafka_catchup) {
        std::error_code ec;
        const fs::path idx_dir = fs::path(fl.db_path) / fl.index_name;
        const fs::path canon   = fs::weakly_canonical(idx_dir, ec);
        if (ec) {
            std::cerr << WallTimestamp() << " kafka_catchup: weakly_canonical: " << ec.message() << "\n";
            DB::Instance().CloseAll();
            return 1;
        }
        yikv_server::TableConfig tcfg;
        try {
            tcfg = yikv_server::LoadTableConfig(canon);
        } catch (const std::exception& e) {
            std::cerr << WallTimestamp() << " kafka_catchup table.json: " << e.what() << "\n";
            DB::Instance().CloseAll();
            return 1;
        }
        if (!tcfg.kafka.has_value()) {
            std::cerr << WallTimestamp() << " kafka_catchup: table.json has no \"kafka\" block\n";
            DB::Instance().CloseAll();
            return 1;
        }
        const auto& kc = *tcfg.kafka;
        std::string brokers =
            kc.brokers.empty() ? fl.kafka_default_brokers : kc.brokers;
        if (brokers.empty()) {
            std::cerr << WallTimestamp()
                      << " kafka_catchup: missing brokers (table.json kafka.brokers or config "
                         "kafka.default_brokers)\n";
            DB::Instance().CloseAll();
            return 1;
        }
        yikv::index::KVIndex* kidx = DB::Instance().GetKVIndex(fl.index_name);
        if (!kidx) {
            std::cerr << WallTimestamp() << " kafka_catchup: index not open\n";
            DB::Instance().CloseAll();
            return 1;
        }
        yikv_server::kafka::KafkaImportCatchupOptions copts;
        copts.brokers                = std::move(brokers);
        copts.topic                  = kc.topic;
        copts.partition              = kc.partition;
        copts.offline_watermark_sec  = fl.kafka_offline_watermark_sec;
        copts.rewind_minutes         = fl.kafka_rewind_minutes;
        if (fl.kafka_catchup_wall_sec > 0) copts.max_wall_seconds = fl.kafka_catchup_wall_sec;
        if (!yikv_server::kafka::RunKafkaImportCatchup(canon, kidx, sch, copts)) {
            DB::Instance().CloseAll();
            return 1;
        }
    }

    DB::Instance().CloseAll();
    return 0;
}
