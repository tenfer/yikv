#pragma once

#include "src/alloc/allocator.h"

#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace yikv {
namespace index {
class KVIndex;
class InvertedIndex;
}  // namespace index
namespace schema {
class Schema;
}  // namespace schema
namespace db {

struct DBOptions {
    std::string                  db_path;
    yikv::alloc::AllocatorOptions alloc_defaults;
};

// Process-wide singleton: call Init once, then Instance().
// Each named index lives under db_path/<name>/ with schema.json, index.meta.json,
// and mmap arena file "arena" (FtAllocator first segment; .segN for growth).
class DB {
public:
    static void Init(DBOptions options);
    static DB&  Instance();
    static void ResetForTest();

    DB(const DB&)            = delete;
    DB& operator=(const DB&) = delete;

    void CreateKVIndex(std::string_view name, const schema::Schema& schema);
    void CreateInvertedIndex(std::string_view name, const schema::Schema& schema);

    // Load an on-disk index into this process (no-op if already open).
    void OpenIndex(std::string_view name);

    index::KVIndex*       GetKVIndex(std::string_view name);
    index::InvertedIndex* GetInvertedIndex(std::string_view name);

    void CloseAll();

private:
    DB() = default;

    struct IndexSlot;  // defined in db.cc
    static void ValidateIndexName(std::string_view name);

    std::string JoinIndexDir(std::string_view name) const;
    std::string SchemaPath(std::string_view name) const;
    std::string MetaPath(std::string_view name) const;
    std::string ArenaPath(std::string_view name) const;

    yikv::alloc::AllocatorOptions ArenaOptionsFor(std::string_view name) const;

    std::mutex                                                 mu_;
    std::string                                                db_path_;
    yikv::alloc::AllocatorOptions                              alloc_defaults_;
    std::unordered_map<std::string, std::unique_ptr<IndexSlot>> indexes_;
};

}  // namespace db
}  // namespace yikv
