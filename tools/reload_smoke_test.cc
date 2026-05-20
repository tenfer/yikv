// Manual / CI smoke test for TableRegistry::ReloadTable (swap + hot-open).
// Run: bazel run //:reload_smoke
//
// Layout: hidden dirs `.v1` / `.v2` / `.v3` hold KV indexes (skipped by ScanAndLoad);
// symlinks `t` / `newt` are the logical table paths under db_path.

#include "table_registry.h"

#include "db/db.h"
#include "index/doc.h"
#include "index/kv_index.h"
#include "schema/schema.h"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

#include <unistd.h>

namespace fs = std::filesystem;
using yikv::db::DB;
using yikv::db::DBOptions;
using yikv::index::Doc;
using yikv::index::KVIndex;
using yikv::schema::Schema;

static const char* kSchJson = R"({
  "table_name": "t",
  "pk": "user_id",
  "fields": [
    {"name": "user_id", "data_type": "int64", "is_pk": true,  "is_index": false, "field_id": 1},
    {"name": "age",     "data_type": "int32", "is_pk": false, "is_index": false, "field_id": 2}
  ]
})";

static void Fail(const std::string& msg) {
    std::cerr << "reload_smoke: " << msg << "\n";
    std::exit(1);
}

static void PutVersion(const Schema& sch, const char* idx, int32_t age) {
    DB::Instance().CreateKVIndex(idx, sch, 10);
    KVIndex* kv = DB::Instance().GetKVIndex(idx);
    Doc      d  = kv->NewDoc();
    d.put_int64(1, 1);
    d.put_int32(2, age);
    kv->Put(&d);
    kv->Publish();
    DB::Instance().CloseIndex(idx);
}

int main() {
    char tmpl[] = "/tmp/yikv_reload_smokeXXXXXX";
    if (::mkdtemp(tmpl) == nullptr) {
        std::perror("mkdtemp");
        return 1;
    }
    const fs::path root(tmpl);

    Schema sch;
    std::string schema_err;
    if (!sch.LoadJson(kSchJson, &schema_err))
        Fail("schema: " + schema_err);

    DBOptions opt;
    opt.db_path                  = root.string();
    opt.exclusive_arena_lock     = false;
    opt.alloc_defaults.mode      = yikv::alloc::AllocatorMode::Concurrent;
    const uint64_t seg         = 64ull * 1024 * 1024;
    opt.alloc_defaults.arena_size     = seg;
    opt.alloc_defaults.segment_size   = seg;
    opt.alloc_defaults.max_arena_size = 256ull * 1024 * 1024;
    DB::Init(std::move(opt));

    PutVersion(sch, ".v1", 10);
    PutVersion(sch, ".v2", 99);

    std::error_code ec;
    fs::create_symlink(".v1", root / "t", ec);
    if (ec)
        Fail("symlink t->.v1: " + ec.message());

    yikv_server::TableRegistry reg(root.string(), "");
    reg.ScanAndLoad();
    if (reg.TableCount() != 1)
        Fail("expected 1 table after scan, got " + std::to_string(reg.TableCount()));

    {
        auto h = reg.Acquire("t");
        if (!h) Fail("Acquire(t)");
        Doc out;
        if (!(*h)->kv->Get("1", &out) || out.get_int32(2) != 10)
            Fail("expected age 10 before reload");
    }

    fs::remove(root / "t", ec);
    fs::create_symlink(".v2", root / "t", ec);
    if (ec)
        Fail("symlink t->.v2: " + ec.message());

    reg.ReloadTable("t");

    {
        auto h = reg.Acquire("t");
        if (!h) Fail("Acquire(t) after reload");
        Doc out;
        if (!(*h)->kv->Get("1", &out) || out.get_int32(2) != 99)
            Fail("expected age 99 after reload");
    }

    PutVersion(sch, ".v3", 5);
    fs::create_symlink(".v3", root / "newt", ec);
    if (ec)
        Fail("symlink newt: " + ec.message());
    reg.ReloadTable("newt");
    {
        auto h = reg.Acquire("newt");
        if (!h) Fail("Acquire(newt)");
        Doc out;
        if (!(*h)->kv->Get("1", &out) || out.get_int32(2) != 5)
            Fail("newt data");
    }

    if (reg.TableCount() != 2)
        Fail("expected 2 tables at end, got " + std::to_string(reg.TableCount()));

    std::cout << "reload_smoke ok\n";
    return 0;
}
