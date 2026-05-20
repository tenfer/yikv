#include "table_registry.h"

#include <chrono>
#include <ctime>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "metrics/metrics.h"
#include "table_config.h"

#include "db/db.h"

namespace yikv_server {

namespace fs = std::filesystem;

static std::string WallTs() {
    auto        now = std::chrono::system_clock::now();
    std::time_t t   = std::chrono::system_clock::to_time_t(now);
    char        buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", std::localtime(&t));
    return buf;
}

#define LOG_INF(msg) std::cerr << "[" << WallTs() << "][registry] " << msg << "\n"
#define LOG_ERR(msg) std::cerr << "[" << WallTs() << "][registry] ERROR: " << msg << "\n"

static constexpr const char kReloadSuffix[] = "~reload";

static bool IsReservedReloadStagingDirName(std::string_view name) {
    return name.size() >= sizeof(kReloadSuffix) - 1 &&
           name.compare(name.size() - (sizeof(kReloadSuffix) - 1), sizeof(kReloadSuffix) - 1,
                        kReloadSuffix) == 0;
}

// ─── ctor / dtor ─────────────────────────────────────────────────────────────

TableRegistry::TableRegistry(fs::path db_path, std::string default_brokers)
    : db_path_(std::move(db_path)), default_brokers_(std::move(default_brokers)) {}

TableRegistry::~TableRegistry() {
    std::unique_lock lk(mu_);
    for (auto& [name, head] : heads_) {
        if (head->slot && head->slot->kafka_src) head->slot->kafka_src->Stop();
    }
}

// ─── ScanAndLoad ─────────────────────────────────────────────────────────────

void TableRegistry::ScanAndLoad() {
    if (!fs::is_directory(db_path_)) {
        LOG_ERR("db_path is not a directory: " << db_path_);
        return;
    }
    for (const auto& entry : fs::directory_iterator(db_path_)) {
        if (!entry.is_directory()) continue;
        const std::string leaf = entry.path().filename().string();
        if (IsReservedReloadStagingDirName(leaf)) continue;
        LoadTable(entry.path());
    }
    LOG_INF("startup scan complete: " << heads_.size() << " table(s) loaded");
}

std::string TableRegistry::ReloadStagingDbName(const std::string& logical_table) {
    return logical_table + kReloadSuffix;
}

std::shared_ptr<TableSlot> TableRegistry::BuildSlotAfterOpen(const std::string& logical_table,
                                                             const std::string&     db_index_name,
                                                             const fs::path& table_config_dir) {
    yikv::index::KVIndex*       kv     = yikv::db::DB::Instance().GetKVIndex(db_index_name);
    const yikv::schema::Schema* schema = kv->schema();
    if (!kv || !schema) {
        LOG_ERR("GetKVIndex(" << db_index_name << ") returned null");
        return nullptr;
    }

    auto slot    = std::make_shared<TableSlot>();
    slot->kv     = kv;
    slot->schema = schema;

    TableConfig tcfg;
    try {
        tcfg = LoadTableConfig(table_config_dir);
    } catch (const std::exception& e) {
        LOG_ERR("table.json error for " << logical_table << ": " << e.what());
    }

    if (tcfg.kafka.has_value()) {
        const auto&        kc      = *tcfg.kafka;
        const std::string& brokers = kc.brokers.empty() ? default_brokers_ : kc.brokers;
        if (brokers.empty()) {
            LOG_ERR("no kafka brokers for table " << logical_table << "; skipping KafkaSource");
        } else {
            std::string offset_file =
                (table_config_dir / "kafka.offset").string();
            slot->kafka_src = std::make_unique<kafka::KafkaSource>(
                kv, schema,
                kafka::KafkaSource::Config{
                    .brokers     = brokers,
                    .topic       = kc.topic,
                    .partition   = kc.partition,
                    .offset_file = std::move(offset_file),
                });
            slot->kafka_src->Start();
            LOG_INF("KafkaSource started for " << logical_table << " topic=" << kc.topic
                                               << " partition=" << kc.partition);
        }
    }

    return slot;
}

// ─── LoadTable ───────────────────────────────────────────────────────────────

bool TableRegistry::LoadTable(const fs::path& table_dir) {
    std::lock_guard<std::mutex> load_lk(load_table_mu_);
    const std::string name = table_dir.filename().string();
    if (name.empty() || name[0] == '.') return false;
    if (IsReservedReloadStagingDirName(name)) return false;

    {
        std::shared_lock lk(mu_);
        if (heads_.count(name)) return true;
    }

    try {
        yikv::db::DB::Instance().OpenIndex(name);
    } catch (const std::exception& e) {
        LOG_ERR("OpenIndex(" << name << ") failed: " << e.what());
        metrics::Metrics::instance().tbl.reload_total.IncFor(name, "error");
        return false;
    }

    std::error_code ec;
    const fs::path  cfg_dir = fs::weakly_canonical(table_dir, ec);
    if (ec) {
        LOG_ERR("weakly_canonical(" << table_dir << ") failed: " << ec.message());
        yikv::db::DB::Instance().CloseIndex(name);
        metrics::Metrics::instance().tbl.reload_total.IncFor(name, "error");
        return false;
    }

    auto slot = BuildSlotAfterOpen(name, name, cfg_dir);
    if (!slot) {
        yikv::db::DB::Instance().CloseIndex(name);
        metrics::Metrics::instance().tbl.reload_total.IncFor(name, "error");
        return false;
    }

    {
        std::unique_lock lk(mu_);
        if (heads_.count(name)) {
            if (slot->kafka_src) slot->kafka_src->Stop();
            yikv::db::DB::Instance().CloseIndex(name);
            return true;
        }
        auto head                = std::make_unique<TableHead>();
        head->slot            = std::move(slot);
        head->staging_is_live = false;
        heads_[name] = std::move(head);
    }
    LOG_INF("loaded table: " << name);
    metrics::Metrics::instance().tbl.reload_total.IncFor(name, "ok");
    return true;
}

// ─── Acquire / Reload ────────────────────────────────────────────────────────

std::optional<TableHandle> TableRegistry::Acquire(const std::string& table_name) {
    std::shared_lock lk(mu_);
    auto             it = heads_.find(table_name);
    if (it == heads_.end()) return std::nullopt;
    TableHead& h = *it->second;
    std::shared_lock hlk(h.mu);
    auto             sp = h.slot;
    if (!sp) return std::nullopt;
    return TableHandle(sp);
}

void TableRegistry::ReloadTable(const std::string& table_name) {
    // RAII outcome tracker. On every exit (return, throw, fall-through) it
    // bumps reload_total{table,result}. The hot-load path that delegates to
    // LoadTable sets `delegated` since LoadTable already counts there.
    struct Outcome {
        std::string name;
        bool        ok        = false;
        bool        delegated = false;
        ~Outcome() {
            if (delegated) return;
            metrics::Metrics::instance().tbl.reload_total.IncFor(
                name, ok ? "ok" : "error");
        }
    } outcome{table_name};

    {
        std::shared_lock lk(mu_);
        if (heads_.find(table_name) == heads_.end()) {
            lk.unlock();
            outcome.delegated = true;
            if (IsReservedReloadStagingDirName(table_name))
                throw std::runtime_error("ReloadTable: reserved name suffix: " + table_name);
            const fs::path dir = db_path_ / table_name;
            if (!fs::is_directory(dir))
                throw std::runtime_error("ReloadTable: not a directory: " + dir.string());
            if (!LoadTable(dir))
                throw std::runtime_error("ReloadTable: failed to open new table: " + table_name);
            LOG_INF("reload: hot-opened new table " << table_name);
            return;
        }
    }

    TableHead* head = nullptr;
    {
        std::shared_lock lk(mu_);
        auto             it = heads_.find(table_name);
        if (it == heads_.end())
            throw std::runtime_error("ReloadTable: race lost table: " + table_name);
        head = it->second.get();
    }

    std::lock_guard<std::mutex> reload_serial_lock(head->reload_serial);

    bool live_on_staging;
    {
        std::shared_lock hlk(head->mu);
        if (!head->slot) throw std::runtime_error("ReloadTable: empty slot: " + table_name);
        live_on_staging = head->staging_is_live;
    }

    const std::string stg               = ReloadStagingDbName(table_name);
    const bool        open_staging_next = !live_on_staging;
    const std::string next_db           = open_staging_next ? stg : table_name;
    const std::string prev_db           = open_staging_next ? table_name : stg;

    const fs::path primary = db_path_ / table_name;
    std::error_code  ec;
    const fs::path   table_config_dir = fs::weakly_canonical(primary, ec);
    if (ec)
        throw std::runtime_error("ReloadTable: weakly_canonical(" + primary.string() + "): " +
                                 ec.message());

    if (next_db != table_name) {
        const fs::path link_path = db_path_ / next_db;
        fs::remove(link_path, ec);
        fs::create_symlink(table_config_dir, link_path, ec);
        if (ec)
            throw std::runtime_error("ReloadTable: create_symlink " + link_path.string() + ": " +
                                     ec.message());
    }

    try {
        // Always drop any stale registration for next_db before opening, so we never hit
        // DB::OpenIndex's early return while the on-disk symlink (e.g. active -> build_id) changed.
        yikv::db::DB::Instance().CloseIndex(next_db);
        yikv::db::DB::Instance().OpenIndex(next_db);
    } catch (...) {
        if (next_db != table_name) {
            fs::remove(db_path_ / next_db, ec);
        }
        throw;
    }

    auto new_slot = BuildSlotAfterOpen(table_name, next_db, table_config_dir);
    if (!new_slot) {
        yikv::db::DB::Instance().CloseIndex(next_db);
        if (next_db != table_name) fs::remove(db_path_ / next_db, ec);
        throw std::runtime_error("ReloadTable: rebuild slot failed: " + table_name);
    }

    std::shared_ptr<TableSlot> old_sp;
    {
        std::unique_lock hlk(head->mu);
        old_sp                  = head->slot;
        head->slot              = std::move(new_slot);
        head->staging_is_live = open_staging_next;
    }

    LOG_INF("reload: live swap " << table_name << " -> db key \"" << next_db << "\"");

    if (old_sp && old_sp->kafka_src) old_sp->kafka_src->Stop();

    for (int i = 0;; ++i) {
        if (old_sp.use_count() == 1) break;
        if (i > 0 && (i % 500) == 0)
            LOG_INF("reload: waiting for in-flight RPCs on old slot " << table_name << "...");
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    old_sp.reset();
    yikv::db::DB::Instance().CloseIndex(prev_db);

    if (prev_db != table_name) {
        fs::remove(db_path_ / prev_db, ec);
        if (ec)
            LOG_ERR("reload: remove " << prev_db << ": " << ec.message());
    }

    LOG_INF("reload: closed previous key \"" << prev_db << "\" for " << table_name);
    outcome.ok = true;
}

}  // namespace yikv_server
