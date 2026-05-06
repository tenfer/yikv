#include "src/db/db.h"

#include <cerrno>
#include <filesystem>
#include <stdexcept>

#include "src/alloc/ft_allocator.h"
#include "src/db/arena_lock.h"
#include "src/db/file_io.h"
#include "src/db/index_meta.h"
#include "src/index/inverted_index.h"
#include "src/index/kv_index.h"
#include "src/schema/schema.h"

namespace yikv {
namespace db {

namespace {

std::mutex          g_db_mutex;
std::unique_ptr<DB> g_db;

constexpr const char kSchemaFile[] = "schema.json";
constexpr const char kMetaFile[]   = "index.meta.json";
constexpr const char kArenaBase[]       = "arena";
constexpr const char kArenaLockFile[] = "arena.lock";

}  // namespace

struct DB::IndexSlot {
    enum class Kind { KV, Inverted };

    std::unique_ptr<ArenaExclusiveLock> arena_lock;
    Kind                                  kind = Kind::KV;
    std::string                           name;
    alloc::FtAllocator                    alloc;
    schema::Schema                        schema;
    std::unique_ptr<index::KVIndex>       kv;
    std::unique_ptr<index::InvertedIndex> inv;

    ~IndexSlot() {
        inv.reset();
        kv.reset();
        if (alloc.IsOpen()) alloc.Close();
    }
};

void DB::ValidateIndexName(std::string_view name) {
    if (name.empty())
        throw std::invalid_argument("DB: index name is empty");
    if (name.find('/') != std::string_view::npos || name.find('\\') != std::string_view::npos)
        throw std::invalid_argument("DB: index name must not contain path separators");
}

std::string DB::JoinIndexDir(std::string_view name) const {
    return db_path_ + "/" + std::string(name);
}

std::string DB::SchemaPath(std::string_view name) const {
    return JoinIndexDir(name) + "/" + kSchemaFile;
}

std::string DB::MetaPath(std::string_view name) const {
    return JoinIndexDir(name) + "/" + kMetaFile;
}

std::string DB::ArenaPath(std::string_view name) const {
    return JoinIndexDir(name) + "/" + kArenaBase;
}

std::string DB::ArenaLockPath(std::string_view name) const {
    return JoinIndexDir(name) + "/" + kArenaLockFile;
}

alloc::AllocatorOptions DB::ArenaOptionsFor(std::string_view name) const {
    alloc::AllocatorOptions o = alloc_defaults_;
    o.path                    = ArenaPath(name);
    return o;
}

void DB::Init(DBOptions options) {
    std::lock_guard<std::mutex> lg(g_db_mutex);
    if (g_db) throw std::logic_error("DB::Init: already initialized");
    if (options.db_path.empty()) throw std::invalid_argument("DB::Init: db_path is empty");
    std::error_code ec;
    std::filesystem::create_directories(options.db_path, ec);
    if (ec)
        throw std::runtime_error("DB::Init: create_directories " + options.db_path + ": " +
                                 ec.message());
    g_db                           = std::unique_ptr<DB>(new DB());
    g_db->db_path_                 = std::move(options.db_path);
    g_db->exclusive_arena_lock_    = options.exclusive_arena_lock;
    g_db->alloc_defaults_         = std::move(options.alloc_defaults);
}

DB& DB::Instance() {
    std::lock_guard<std::mutex> lg(g_db_mutex);
    if (!g_db) throw std::logic_error("DB::Instance: DB::Init not called");
    return *g_db;
}

void DB::ResetForTest() {
    std::lock_guard<std::mutex> lg(g_db_mutex);
    if (!g_db) return;
    {
        std::lock_guard<std::mutex> ilock(g_db->mu_);
        g_db->indexes_.clear();
    }
    g_db.reset();
}

void DB::CreateKVIndex(std::string_view name, const schema::Schema& schema) {
    ValidateIndexName(name);
    std::lock_guard<std::mutex> lock(mu_);

    const std::string n(name);
    if (indexes_.count(n)) throw std::runtime_error("DB::CreateKVIndex: index already open: " + n);

    const std::string dir = JoinIndexDir(name);
    std::error_code ec;
    if (std::filesystem::exists(dir, ec))
        throw std::runtime_error("DB::CreateKVIndex: directory already exists: " + dir);

    std::filesystem::create_directory(dir, ec);
    if (ec)
        throw std::runtime_error("DB::CreateKVIndex: create_directory " + dir + ": " + ec.message());

    AtomicWriteFile(SchemaPath(name), schema.ToJson());

    auto slot    = std::make_unique<IndexSlot>();
    slot->kind   = IndexSlot::Kind::KV;
    slot->name   = n;
    {
        std::string err_copy;
        if (!slot->schema.LoadJson(schema.ToJson(), &err_copy))
            throw std::runtime_error("DB::CreateKVIndex: schema copy: " + err_copy);
    }
    if (exclusive_arena_lock_) {
        slot->arena_lock = std::make_unique<ArenaExclusiveLock>(ArenaLockPath(name));
    }
    slot->alloc.Open(ArenaOptionsFor(name));
    slot->kv = std::make_unique<index::KVIndex>(&slot->alloc, &slot->schema, 0, 0);

    IndexMeta meta;
    meta.kind            = IndexKind::KV;
    meta.index_hdr_off   = slot->kv->index_hdr_offset();
    meta.docs_hdr_off    = slot->kv->docs_root_offset();
    meta.posting_hdr_off = 0;
    AtomicWriteFile(MetaPath(name), SerializeIndexMeta(meta));

    indexes_.emplace(n, std::move(slot));
}

void DB::CreateInvertedIndex(std::string_view name, const schema::Schema& schema) {
    ValidateIndexName(name);
    std::lock_guard<std::mutex> lock(mu_);

    const std::string n(name);
    if (indexes_.count(n))
        throw std::runtime_error("DB::CreateInvertedIndex: index already open: " + n);

    const std::string dir = JoinIndexDir(name);
    std::error_code ec;
    if (std::filesystem::exists(dir, ec))
        throw std::runtime_error("DB::CreateInvertedIndex: directory already exists: " + dir);

    std::filesystem::create_directory(dir, ec);
    if (ec)
        throw std::runtime_error("DB::CreateInvertedIndex: create_directory " + dir + ": " +
                                 ec.message());

    AtomicWriteFile(SchemaPath(name), schema.ToJson());

    auto slot  = std::make_unique<IndexSlot>();
    slot->kind = IndexSlot::Kind::Inverted;
    slot->name = n;
    {
        std::string err_copy;
        if (!slot->schema.LoadJson(schema.ToJson(), &err_copy))
            throw std::runtime_error("DB::CreateInvertedIndex: schema copy: " + err_copy);
    }
    if (exclusive_arena_lock_) {
        slot->arena_lock = std::make_unique<ArenaExclusiveLock>(ArenaLockPath(name));
    }
    slot->alloc.Open(ArenaOptionsFor(name));
    slot->inv =
        std::make_unique<index::InvertedIndex>(&slot->alloc, &slot->schema, 0, 0, 0);

    IndexMeta meta;
    meta.kind            = IndexKind::Inverted;
    meta.index_hdr_off   = slot->inv->index_hdr_offset();
    meta.docs_hdr_off    = slot->inv->docs_root_offset();
    meta.posting_hdr_off = slot->inv->posting_root_offset();
    AtomicWriteFile(MetaPath(name), SerializeIndexMeta(meta));

    indexes_.emplace(n, std::move(slot));
}

void DB::OpenIndex(std::string_view name) {
    ValidateIndexName(name);
    std::lock_guard<std::mutex> lock(mu_);

    const std::string n(name);
    if (indexes_.count(n)) return;

    const std::string dir = JoinIndexDir(name);
    if (!std::filesystem::is_directory(dir))
        throw std::runtime_error("DB::OpenIndex: not a directory: " + dir);

    auto slot = std::make_unique<IndexSlot>();
    slot->name = n;

    std::string schema_err;
    if (!slot->schema.LoadJson(ReadWholeFile(SchemaPath(name)), &schema_err))
        throw std::runtime_error("DB::OpenIndex: schema: " + schema_err);

    IndexMeta meta;
    std::string meta_err;
    if (!LoadIndexMeta(ReadWholeFile(MetaPath(name)), &meta, &meta_err))
        throw std::runtime_error("DB::OpenIndex: meta: " + meta_err);

    if (exclusive_arena_lock_) {
        slot->arena_lock = std::make_unique<ArenaExclusiveLock>(ArenaLockPath(name));
    }
    slot->alloc.Open(ArenaOptionsFor(name));

    switch (meta.kind) {
        case IndexKind::KV:
            slot->kind = IndexSlot::Kind::KV;
            slot->kv   = std::make_unique<index::KVIndex>(
                &slot->alloc, &slot->schema, meta.index_hdr_off, meta.docs_hdr_off);
            break;
        case IndexKind::Inverted:
            slot->kind = IndexSlot::Kind::Inverted;
            slot->inv  = std::make_unique<index::InvertedIndex>(
                &slot->alloc, &slot->schema, meta.index_hdr_off, meta.docs_hdr_off,
                meta.posting_hdr_off);
            break;
    }

    indexes_.emplace(n, std::move(slot));
}

index::KVIndex* DB::GetKVIndex(std::string_view name) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = indexes_.find(std::string(name));
    if (it == indexes_.end()) throw std::runtime_error("DB::GetKVIndex: unknown index");
    if (it->second->kind != IndexSlot::Kind::KV)
        throw std::runtime_error("DB::GetKVIndex: index is not KV");
    return it->second->kv.get();
}

index::InvertedIndex* DB::GetInvertedIndex(std::string_view name) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = indexes_.find(std::string(name));
    if (it == indexes_.end()) throw std::runtime_error("DB::GetInvertedIndex: unknown index");
    if (it->second->kind != IndexSlot::Kind::Inverted)
        throw std::runtime_error("DB::GetInvertedIndex: index is not inverted");
    return it->second->inv.get();
}

void DB::CloseAll() {
    std::lock_guard<std::mutex> lock(mu_);
    indexes_.clear();
}

}  // namespace db
}  // namespace yikv
