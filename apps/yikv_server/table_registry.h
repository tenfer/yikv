#pragma once

// TableRegistry — manages all loaded KV tables for yikv_server.
//
// Responsibilities:
//  1. Startup scan: load every sub-directory of db_path that contains a
//     valid arena (opened via DB::OpenIndex). Tables that appear later can be
//     opened with ReloadTable (admin «reload»), same entry path as LoadTable.
//  2. RPC routing: Acquire(table_name) returns a TableHandle that pins the
//     TableSlot (shared_ptr) for the duration of the request.
//  3. ReloadTable(table_name): after updating «active», opens the new tree
//     under an alternate DB key (name vs name~reload), swaps the live slot,
//     then after in-flight RPCs release their handles closes the old mmap.
//     New requests see the new index immediately after the swap; there is no
//     window where the table has no index (unlike close-then-open on one key).
//
// Thread-safety:
//  - load_table_mu: serializes LoadTable (startup scan vs admin hot-open).
//  - head.mu: protects slot pointer / staging_is_live for brief read/write.
//  - reload_serial: only one ReloadTable at a time per table.
//  - Each table's write_mu serialises KVIndex mutations (single-writer rule).

#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>

#include "kafka/kafka_source.h"

#include "index/kv_index.h"
#include "schema/schema.h"

namespace yikv_server {

struct TableSlot {
    yikv::index::KVIndex*       kv     = nullptr;
    const yikv::schema::Schema* schema = nullptr;
    std::mutex                  write_mu;
    std::unique_ptr<kafka::KafkaSource> kafka_src;

    TableSlot() = default;
    TableSlot(const TableSlot&) = delete;
    TableSlot& operator=(const TableSlot&) = delete;
};

// RAII: keeps TableSlot alive for the whole RPC (reload may mmap-swap underneath).
class TableHandle {
public:
    explicit TableHandle(std::shared_ptr<TableSlot> slot);

    TableHandle(const TableHandle&) = default;
    TableHandle& operator=(const TableHandle&) = default;
    TableHandle(TableHandle&&)                 = default;
    TableHandle& operator=(TableHandle&&)     = default;

    explicit operator bool() const { return sp_ != nullptr; }
    TableSlot* operator->() const { return sp_.get(); }
    TableSlot& operator*() const { return *sp_; }

private:
    std::shared_ptr<TableSlot> sp_;
};

struct TableHead {
    // Protects `slot` / `staging_is_live` (short critical sections).
    std::shared_mutex mu;
    // Serializes ReloadTable for this logical table.
    std::mutex reload_serial;
    // If true, the mmap registered in DB under (logical_name + "~reload") is
    // the live one; if false, under logical_name.
    bool                         staging_is_live = false;
    std::shared_ptr<TableSlot> slot;
};

class TableRegistry {
public:
    explicit TableRegistry(std::filesystem::path db_path,
                           std::string default_brokers = {});
    ~TableRegistry();

    void ScanAndLoad();

    std::optional<TableHandle> Acquire(const std::string& table_name);

    // After «active» points at a new build: open new mmap, swap live slot, drain
    // RPCs on the old slot, then CloseIndex the old DB key. If the table is not
    // yet registered, performs a first open (same as startup LoadTable).
    void ReloadTable(const std::string& table_name);

    template <typename Fn>
    void ForEach(Fn&& fn) {
        std::shared_lock lk(mu_);
        for (auto& [name, head] : heads_) {
            if (head->slot) fn(name, *head->slot);
        }
    }

    size_t TableCount() const {
        std::shared_lock lk(mu_);
        return heads_.size();
    }

private:
    static std::string ReloadStagingDbName(const std::string& logical_table);

    bool LoadTable(const std::filesystem::path& table_dir);

    std::shared_ptr<TableSlot> BuildSlotAfterOpen(const std::string& logical_table,
                                                  const std::string& db_index_name,
                                                  const std::filesystem::path& table_config_dir);

    std::filesystem::path db_path_;
    std::string           default_brokers_;

    mutable std::shared_mutex mu_;
    // Serializes LoadTable to avoid races between ScanAndLoad and admin ReloadTable hot-open.
    std::mutex load_table_mu_;
    std::unordered_map<std::string, std::unique_ptr<TableHead>> heads_;
};

inline TableHandle::TableHandle(std::shared_ptr<TableSlot> slot)
    : sp_(std::move(slot)) {}

}  // namespace yikv_server
