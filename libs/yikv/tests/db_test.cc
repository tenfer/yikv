#include "db/db.h"

#include "container/bitmap.h"
#include "index/inverted_index.h"
#include "index/kv_index.h"
#include "schema/schema.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

using yikv::alloc::AllocatorOptions;
using yikv::db::DB;
using yikv::db::DBOptions;
using yikv::index::Doc;
using yikv::index::InvertedIndex;
using yikv::index::KVIndex;
using yikv::schema::Schema;

namespace {

static const char* kKVSchemaJson = R"({
  "table_name": "user",
  "pk": "user_id",
  "fields": [
    {"name": "user_id", "data_type": "int64", "is_pk": true,  "is_index": false, "field_id": 1},
    {"name": "age",     "data_type": "int32", "is_pk": false, "is_index": false, "field_id": 2},
    {"name": "name",    "data_type": "string","is_pk": false, "is_index": false, "field_id": 3}
  ]
})";

static const char* kInvSchemaJson = R"({
  "table_name": "user",
  "pk": "user_id",
  "fields": [
    {"name": "user_id", "data_type": "int64", "is_pk": true,  "is_index": false, "field_id": 1},
    {"name": "bio",     "data_type": "string","is_pk": false, "is_index": true,  "field_id": 2}
  ]
})";

// String primary key (KV index).
static const char* kStringPkSchemaJson = R"({
  "table_name": "session",
  "pk": "sid",
  "fields": [
    {"name": "sid",   "data_type": "string", "is_pk": true,  "is_index": false, "field_id": 1},
    {"name": "epoch", "data_type": "int64",  "is_pk": false, "is_index": false, "field_id": 2}
  ]
})";

constexpr uint32_t kFidUserId = 1;
constexpr uint32_t kFidAge    = 2;
constexpr uint32_t kFidName   = 3;
constexpr uint32_t kFidBio    = 2;
constexpr uint32_t kFidSid    = 1;
constexpr uint32_t kFidEpoch  = 2;

std::string MakeTempDbRoot() {
    namespace fs = std::filesystem;
    std::string base =
        (fs::temp_directory_path() / "yikv_db_XXXXXX").string();
    std::vector<char> buf(base.begin(), base.end());
    buf.push_back('\0');
    if (::mkdtemp(buf.data()) == nullptr) {
        throw std::runtime_error("mkdtemp failed");
    }
    return std::string(buf.data());
}

AllocatorOptions TestArenaOpts() {
    AllocatorOptions o;
    o.arena_size        = 64 * 1024 * 1024;
    o.reclaim_delay_ns  = 0;  // immediate CoW retirement under churn
    return o;
}

}  // namespace

class DBTest : public ::testing::Test {
protected:
    void SetUp() override {
        DB::ResetForTest();
        root_ = MakeTempDbRoot();
        DBOptions opt;
        opt.db_path          = root_;
        opt.alloc_defaults   = TestArenaOpts();
        DB::Init(std::move(opt));

        std::string err;
        ASSERT_TRUE(kv_schema_.LoadJson(kKVSchemaJson, &err)) << err;
        ASSERT_TRUE(inv_schema_.LoadJson(kInvSchemaJson, &err)) << err;
        ASSERT_TRUE(string_pk_schema_.LoadJson(kStringPkSchemaJson, &err)) << err;
    }

    void TearDown() override {
        DB::ResetForTest();
        std::error_code ec;
        std::filesystem::remove_all(root_, ec);
    }

    std::string root_;
    Schema      kv_schema_;
    Schema      inv_schema_;
    Schema      string_pk_schema_;
};

TEST_F(DBTest, CreateKVAndRecover) {
    DB::Instance().CreateKVIndex("main", kv_schema_);

    KVIndex* idx = DB::Instance().GetKVIndex("main");
    Doc doc      = idx->NewDoc();
    doc.put_int64(kFidUserId, 42);
    doc.put_int32(kFidAge, 7);
    doc.put_string(kFidName, "alice");
    idx->Put(&doc);
    idx->Publish();

    DB::Instance().CloseAll();
    DB::ResetForTest();

    DBOptions opt;
    opt.db_path        = root_;
    opt.alloc_defaults = TestArenaOpts();
    DB::Init(std::move(opt));
    DB::Instance().OpenIndex("main");

    KVIndex* again = DB::Instance().GetKVIndex("main");
    Doc      out;
    ASSERT_TRUE(again->Get("42", &out));
    EXPECT_EQ(out.get_int64(kFidUserId), 42);
    EXPECT_EQ(out.get_int32(kFidAge), 7);
    EXPECT_EQ(std::string(out.get_string(kFidName)), "alice");
}

TEST_F(DBTest, CreateInvertedAndRecover) {
    DB::Instance().CreateInvertedIndex("inv", inv_schema_);

    InvertedIndex* idx = DB::Instance().GetInvertedIndex("inv");
    Doc              d = idx->NewDoc();
    uint32_t         did = d.doc_id();
    d.put_int64(kFidUserId, 100);
    d.put_string(kFidBio, "hello world");
    idx->Put(&d);
    idx->Publish();

    yikv::container::Bitmap bm(idx->alloc(), 0);
    ASSERT_TRUE(idx->Query(kFidBio, "hello", &bm));
    EXPECT_TRUE(bm.Contains(did));

    DB::Instance().CloseAll();
    DB::ResetForTest();

    DBOptions opt;
    opt.db_path        = root_;
    opt.alloc_defaults = TestArenaOpts();
    DB::Init(std::move(opt));
    DB::Instance().OpenIndex("inv");

    InvertedIndex* again = DB::Instance().GetInvertedIndex("inv");
    Doc            out;
    ASSERT_TRUE(again->Get("100", &out));
    EXPECT_EQ(out.get_int64(kFidUserId), 100);

    yikv::container::Bitmap bm2(again->alloc(), 0);
    ASSERT_TRUE(again->Query(kFidBio, "world", &bm2));
    EXPECT_TRUE(bm2.Contains(did));
}

// ─── Init / API guards (no DBTest fixture: singleton must start clean) ─────────

TEST(DB_InitErrors, InstanceWithoutInitThrows) {
    DB::ResetForTest();
    EXPECT_THROW(DB::Instance(), std::logic_error);
}

TEST(DB_InitErrors, EmptyDbPathThrows) {
    DB::ResetForTest();
    DBOptions opt;
    opt.db_path = "";
    EXPECT_THROW(DB::Init(std::move(opt)), std::invalid_argument);
}

TEST(DB_InitErrors, DoubleInitThrows) {
    DB::ResetForTest();
    std::string root = MakeTempDbRoot();
    DBOptions opt;
    opt.db_path        = root;
    opt.alloc_defaults = TestArenaOpts();
    DB::Init(std::move(opt));
    DBOptions opt2;
    opt2.db_path        = root;
    opt2.alloc_defaults = TestArenaOpts();
    EXPECT_THROW(DB::Init(std::move(opt2)), std::logic_error);
    DB::Instance().CloseAll();
    DB::ResetForTest();
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

// ─── DBTest: naming, errors, two index kinds ─────────────────────────────────

TEST_F(DBTest, EmptyIndexNameThrows) {
    EXPECT_THROW(DB::Instance().CreateKVIndex("", kv_schema_), std::invalid_argument);
    EXPECT_THROW(DB::Instance().CreateInvertedIndex("", inv_schema_), std::invalid_argument);
}

TEST_F(DBTest, IndexNameWithSlashThrows) {
    EXPECT_THROW(DB::Instance().CreateKVIndex("a/b", kv_schema_), std::invalid_argument);
}

TEST_F(DBTest, CreateKVWhenDirectoryAlreadyExistsThrows) {
    namespace fs = std::filesystem;
    fs::create_directories(fs::path(root_) / "blocked");
    EXPECT_THROW(DB::Instance().CreateKVIndex("blocked", kv_schema_), std::runtime_error);
}

TEST_F(DBTest, DuplicateCreateKVIndexThrows) {
    DB::Instance().CreateKVIndex("once", kv_schema_);
    EXPECT_THROW(DB::Instance().CreateKVIndex("once", kv_schema_), std::runtime_error);
}

TEST_F(DBTest, GetKVIndexOnInvertedOnlyThrows) {
    DB::Instance().CreateInvertedIndex("invonly", inv_schema_);
    EXPECT_THROW(DB::Instance().GetKVIndex("invonly"), std::runtime_error);
}

TEST_F(DBTest, GetInvertedIndexOnKVOnlyThrows) {
    DB::Instance().CreateKVIndex("kvonly", kv_schema_);
    EXPECT_THROW(DB::Instance().GetInvertedIndex("kvonly"), std::runtime_error);
}

TEST_F(DBTest, OpenUnknownIndexThrows) {
    EXPECT_THROW(DB::Instance().OpenIndex("does_not_exist"), std::runtime_error);
}

TEST_F(DBTest, UnknownKVIndexNameThrows) {
    EXPECT_THROW(DB::Instance().GetKVIndex("missing"), std::runtime_error);
}

TEST_F(DBTest, TwoIndexesKvAndInvertedCoexist) {
    DB::Instance().CreateKVIndex("kv", kv_schema_);
    DB::Instance().CreateInvertedIndex("inv", inv_schema_);

    KVIndex* kv = DB::Instance().GetKVIndex("kv");
    Doc        kd = kv->NewDoc();
    kd.put_int64(kFidUserId, 7);
    kd.put_int32(kFidAge, 21);
    kd.put_string(kFidName, "bob");
    kv->Put(&kd);
    kv->Publish();

    InvertedIndex* iv = DB::Instance().GetInvertedIndex("inv");
    Doc            id = iv->NewDoc();
    id.put_int64(kFidUserId, 99);
    id.put_string(kFidBio, "foo bar");
    iv->Put(&id);
    iv->Publish();

    Doc kv_out;
    ASSERT_TRUE(kv->Get("7", &kv_out));
    EXPECT_EQ(kv_out.get_string(kFidName), "bob");

    yikv::container::Bitmap bm(iv->alloc(), 0);
    ASSERT_TRUE(iv->Query(kFidBio, "foo", &bm));
    EXPECT_TRUE(bm.Contains(id.doc_id()));
}

TEST_F(DBTest, OpenIndexIdempotent) {
    DB::Instance().CreateKVIndex("main", kv_schema_);
    DB::Instance().OpenIndex("main");
    DB::Instance().OpenIndex("main");
    KVIndex* idx = DB::Instance().GetKVIndex("main");
    Doc      d   = idx->NewDoc();
    d.put_int64(kFidUserId, 3);
    idx->Put(&d);
    idx->Publish();
    Doc out;
    ASSERT_TRUE(idx->Get("3", &out));
}

TEST_F(DBTest, CloseIndexThenReopen) {
    DB::Instance().CreateKVIndex("main", kv_schema_);
    KVIndex* idx = DB::Instance().GetKVIndex("main");
    Doc      d   = idx->NewDoc();
    d.put_int64(kFidUserId, 9);
    idx->Put(&d);
    idx->Publish();

    DB::Instance().CloseIndex("main");
    DB::Instance().OpenIndex("main");
    idx = DB::Instance().GetKVIndex("main");
    Doc out;
    ASSERT_TRUE(idx->Get("9", &out));
}

// ─── KV rows: upsert, delete, batch, miss, string PK ───────────────────────────

TEST_F(DBTest, GetMissReturnsFalse) {
    DB::Instance().CreateKVIndex("main", kv_schema_);
    KVIndex* idx = DB::Instance().GetKVIndex("main");
    Doc      out;
    EXPECT_FALSE(idx->Get("999", &out));
}

TEST_F(DBTest, UpsertSamePrimaryKeyReplacesRow) {
    DB::Instance().CreateKVIndex("main", kv_schema_);
    KVIndex* idx = DB::Instance().GetKVIndex("main");
    Doc      d1  = idx->NewDoc();
    d1.put_int64(kFidUserId, 1);
    d1.put_int32(kFidAge, 10);
    d1.put_string(kFidName, "v1");
    idx->Put(&d1);
    idx->Publish();

    Doc d2 = idx->NewDoc();
    d2.put_int64(kFidUserId, 1);
    d2.put_int32(kFidAge, 11);
    d2.put_string(kFidName, "v2");
    idx->Upsert(&d2);
    idx->Publish();

    Doc out;
    ASSERT_TRUE(idx->Get("1", &out));
    EXPECT_EQ(out.get_int32(kFidAge), 11);
    EXPECT_EQ(std::string(out.get_string(kFidName)), "v2");
}

TEST_F(DBTest, DeleteRemovesRowAndPersistsAfterReopen) {
    DB::Instance().CreateKVIndex("main", kv_schema_);
    KVIndex* idx = DB::Instance().GetKVIndex("main");
    Doc      d   = idx->NewDoc();
    d.put_int64(kFidUserId, 55);
    d.put_int32(kFidAge, 1);
    idx->Put(&d);
    idx->Publish();
    ASSERT_TRUE(idx->Delete("55"));

    Doc miss;
    EXPECT_FALSE(idx->Get("55", &miss));

    DB::Instance().CloseAll();
    DB::ResetForTest();
    DBOptions opt;
    opt.db_path        = root_;
    opt.alloc_defaults = TestArenaOpts();
    DB::Init(std::move(opt));
    DB::Instance().OpenIndex("main");

    KVIndex* again = DB::Instance().GetKVIndex("main");
    Doc      out;
    EXPECT_FALSE(again->Get("55", &out));
}

TEST_F(DBTest, BatchPutPublishAndRecover) {
    DB::Instance().CreateKVIndex("batch", kv_schema_);
    KVIndex* idx = DB::Instance().GetKVIndex("batch");

    std::vector<Doc>    docs;
    std::vector<Doc*>   ptrs;
    constexpr int       kN = 80;
    docs.reserve(kN);
    ptrs.reserve(kN);
    for (int i = 0; i < kN; ++i) {
        docs.push_back(idx->NewDoc());
        Doc* p = &docs.back();
        p->put_int64(kFidUserId, static_cast<int64_t>(1000 + i));
        p->put_int32(kFidAge, static_cast<int32_t>(i));
        p->put_string(kFidName, "row");
        ptrs.push_back(p);
    }
    idx->BatchPut(ptrs);
    idx->Publish();

    DB::Instance().CloseAll();
    DB::ResetForTest();
    DBOptions opt;
    opt.db_path        = root_;
    opt.alloc_defaults = TestArenaOpts();
    DB::Init(std::move(opt));
    DB::Instance().OpenIndex("batch");

    KVIndex* again = DB::Instance().GetKVIndex("batch");
    Doc      out;
    ASSERT_TRUE(again->Get("1077", &out));
    EXPECT_EQ(out.get_int32(kFidAge), 77);
}

TEST_F(DBTest, StringPrimaryKeyRoundTrip) {
    DB::Instance().CreateKVIndex("sess", string_pk_schema_);
    KVIndex* idx = DB::Instance().GetKVIndex("sess");
    Doc      d    = idx->NewDoc();
    d.put_string(kFidSid, "sess-abc");
    d.put_int64(kFidEpoch, 9001);
    idx->Put(&d);
    idx->Publish();

    Doc out;
    ASSERT_TRUE(idx->Get("sess-abc", &out));
    EXPECT_EQ(std::string(out.get_string(kFidSid)), "sess-abc");
    EXPECT_EQ(out.get_int64(kFidEpoch), 9001);
}

TEST_F(DBTest, StressManyKvDocumentsRecover) {
    DB::Instance().CreateKVIndex("stress", kv_schema_);
    KVIndex* idx = DB::Instance().GetKVIndex("stress");
    constexpr int kRows = 600;
    for (int i = 0; i < kRows; ++i) {
        Doc d = idx->NewDoc();
        d.put_int64(kFidUserId, static_cast<int64_t>(i));
        d.put_int32(kFidAge, static_cast<int32_t>(i % 128));
        d.put_string(kFidName, "u");
        idx->Put(&d);
    }
    idx->Publish();
    ASSERT_EQ(idx->Size(), static_cast<size_t>(kRows));

    DB::Instance().CloseAll();
    DB::ResetForTest();
    DBOptions opt;
    opt.db_path        = root_;
    opt.alloc_defaults = TestArenaOpts();
    DB::Init(std::move(opt));
    DB::Instance().OpenIndex("stress");

    KVIndex* again = DB::Instance().GetKVIndex("stress");
    ASSERT_EQ(again->Size(), static_cast<size_t>(kRows));
    for (int check : {0, 1, 299, 598}) {
        Doc out;
        ASSERT_TRUE(again->Get(std::to_string(check), &out)) << check;
        EXPECT_EQ(out.get_int64(kFidUserId), static_cast<int64_t>(check));
        EXPECT_EQ(out.get_int32(kFidAge), check % 128);
    }
}

// ─── Inverted: QueryAnd / QueryOr / delete + reopen ────────────────────────────

TEST_F(DBTest, InvertedQueryAndAndQueryOr) {
    DB::Instance().CreateInvertedIndex("inv", inv_schema_);
    InvertedIndex* idx = DB::Instance().GetInvertedIndex("inv");

    Doc a = idx->NewDoc();
    a.put_int64(kFidUserId, 1);
    a.put_string(kFidBio, "quick brown fox");
    idx->Put(&a);

    Doc b = idx->NewDoc();
    b.put_int64(kFidUserId, 2);
    b.put_string(kFidBio, "lazy dog jumps");
    idx->Put(&b);
    idx->Publish();

    yikv::container::Bitmap and_bm =
        idx->QueryAnd(kFidBio, std::vector<std::string>{"quick", "brown"});
    EXPECT_TRUE(and_bm.Contains(a.doc_id()));
    EXPECT_FALSE(and_bm.Contains(b.doc_id()));

    yikv::container::Bitmap or_bm =
        idx->QueryOr(kFidBio, std::vector<std::string>{"fox", "lazy"});
    EXPECT_TRUE(or_bm.Contains(a.doc_id()));
    EXPECT_TRUE(or_bm.Contains(b.doc_id()));
}

TEST_F(DBTest, InvertedDeleteRemovesFromPostingsAndPersists) {
    DB::Instance().CreateInvertedIndex("inv", inv_schema_);
    InvertedIndex* idx = DB::Instance().GetInvertedIndex("inv");
    Doc            d   = idx->NewDoc();
    uint32_t       did = d.doc_id();
    d.put_int64(kFidUserId, 500);
    d.put_string(kFidBio, "unique token xyz");
    idx->Put(&d);
    idx->Publish();

    yikv::container::Bitmap bm(idx->alloc(), 0);
    ASSERT_TRUE(idx->Query(kFidBio, "unique", &bm));
    EXPECT_TRUE(bm.Contains(did));

    ASSERT_TRUE(idx->Delete("500"));

    yikv::container::Bitmap bm2(idx->alloc(), 0);
    EXPECT_FALSE(idx->Query(kFidBio, "unique", &bm2));

    DB::Instance().CloseAll();
    DB::ResetForTest();
    DBOptions opt;
    opt.db_path        = root_;
    opt.alloc_defaults = TestArenaOpts();
    DB::Init(std::move(opt));
    DB::Instance().OpenIndex("inv");

    InvertedIndex* again = DB::Instance().GetInvertedIndex("inv");
    yikv::container::Bitmap bm3(again->alloc(), 0);
    EXPECT_FALSE(again->Query(kFidBio, "unique", &bm3));
    Doc miss;
    EXPECT_FALSE(again->Get("500", &miss));
}

TEST_F(DBTest, OpenIndexFailsWhenArenaLockHeldExternally) {
    DB::Instance().CreateKVIndex("locked", kv_schema_);
    DB::Instance().CloseAll();

    const std::string lock_path = root_ + "/locked/arena.lock";
    int               fd        = ::open(lock_path.c_str(), O_RDWR | O_CREAT, 0644);
    ASSERT_GE(fd, 0);
    ASSERT_EQ(::flock(fd, LOCK_EX), 0);

    EXPECT_THROW(DB::Instance().OpenIndex("locked"), std::runtime_error);

    ASSERT_EQ(::close(fd), 0);
    ASSERT_NO_THROW(DB::Instance().OpenIndex("locked"));
    KVIndex* idx = DB::Instance().GetKVIndex("locked");
    ASSERT_NE(idx, nullptr);
}

TEST_F(DBTest, OpenIndexBypassesLockWhenExclusiveArenaLockDisabled) {
    DB::Instance().CreateKVIndex("nolock", kv_schema_);
    DB::Instance().CloseAll();
    DB::ResetForTest();

    DBOptions opt;
    opt.db_path                 = root_;
    opt.alloc_defaults          = TestArenaOpts();
    opt.exclusive_arena_lock    = false;
    DB::Init(std::move(opt));

    const std::string lock_path = root_ + "/nolock/arena.lock";
    int               fd        = ::open(lock_path.c_str(), O_RDWR | O_CREAT, 0644);
    ASSERT_GE(fd, 0);
    ASSERT_EQ(::flock(fd, LOCK_EX), 0);

    EXPECT_NO_THROW(DB::Instance().OpenIndex("nolock"));
    ASSERT_EQ(::close(fd), 0);
}

TEST_F(DBTest, ReopenAfterCloseAllInSameProcess) {
    DB::Instance().CreateKVIndex("r", kv_schema_);
    DB::Instance().CloseAll();
    ASSERT_NO_THROW(DB::Instance().OpenIndex("r"));
    KVIndex* idx = DB::Instance().GetKVIndex("r");
    ASSERT_NE(idx, nullptr);
}
