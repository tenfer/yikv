#include "index/inverted_index.h"
#include "alloc/ft_allocator.h"
#include "schema/schema.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using yikv::alloc::AllocatorOptions;
using yikv::alloc::FtAllocator;
using yikv::index::Doc;
using yikv::index::InvertedIndex;
using yikv::schema::Schema;

// Schema: user_id(int64, pk, fid=1), age(int32, is_index, fid=2),
//         bio(string, is_index, fid=3)
static const char* kTestSchemaJson = R"({
  "table_name": "user",
  "pk": "user_id",
  "fields": [
    {"name": "user_id", "data_type": "int64", "is_pk": true,  "is_index": false, "field_id": 1},
    {"name": "age",     "data_type": "int32", "is_pk": false, "is_index": true,  "field_id": 2},
    {"name": "bio",     "data_type": "string","is_pk": false, "is_index": true,  "field_id": 3}
  ]
})";

static constexpr uint32_t kFidUserId = 1;
static constexpr uint32_t kFidAge    = 2;
static constexpr uint32_t kFidBio    = 3;

static AllocatorOptions AnonArena(std::size_t size = 64 * 1024 * 1024) {
    AllocatorOptions opts;
    opts.arena_size = size;
    return opts;
}

class InvertedIndexTest : public ::testing::Test {
protected:
    void SetUp() override {
        alloc.Open(AnonArena());
        std::string err;
        ASSERT_TRUE(schema.LoadJson(kTestSchemaJson, &err)) << err;
    }

    FtAllocator alloc;
    Schema      schema;
};

// ── Basic doc put + term query ────────────────────────────────────────────────

TEST_F(InvertedIndexTest, PutDocAndQueryByStringTerm) {
    InvertedIndex idx(&alloc, &schema);

    Doc d = idx.NewDoc();
    d.put_int64(kFidUserId, 1);
    d.put_int32(kFidAge, 30);
    d.put_string(kFidBio, "loves hiking and running");
    idx.Put(&d);

    yikv::container::Bitmap bm(&alloc, 0);
    ASSERT_TRUE(idx.Query(kFidBio, "hiking", &bm));
    EXPECT_TRUE(bm.Contains(d.doc_id()));
    EXPECT_EQ(bm.Cardinality(), 1u);
}

TEST_F(InvertedIndexTest, PutDocAndQueryByIntTerm) {
    InvertedIndex idx(&alloc, &schema);

    Doc d = idx.NewDoc();
    d.put_int64(kFidUserId, 2);
    d.put_int32(kFidAge, 25);
    d.put_string(kFidBio, "");
    idx.Put(&d);

    yikv::container::Bitmap bm(&alloc, 0);
    ASSERT_TRUE(idx.Query(kFidAge, "25", &bm));
    EXPECT_TRUE(bm.Contains(d.doc_id()));
}

// ── Multiple docs, AND / OR queries ──────────────────────────────────────────

TEST_F(InvertedIndexTest, AndOrQueries) {
    InvertedIndex idx(&alloc, &schema);

    // doc A: bio = "rock climbing"
    Doc da = idx.NewDoc();
    da.put_int64(kFidUserId, 10);
    da.put_string(kFidBio, "rock climbing");
    idx.Put(&da);

    // doc B: bio = "rock music"
    Doc db = idx.NewDoc();
    db.put_int64(kFidUserId, 11);
    db.put_string(kFidBio, "rock music");
    idx.Put(&db);

    // doc C: bio = "jazz music"
    Doc dc = idx.NewDoc();
    dc.put_int64(kFidUserId, 12);
    dc.put_string(kFidBio, "jazz music");
    idx.Put(&dc);

    uint32_t id_a = da.doc_id(), id_b = db.doc_id(), id_c = dc.doc_id();

    // AND("rock", "music") → only B
    auto and_bm = idx.QueryAnd(kFidBio, {"rock", "music"});
    EXPECT_FALSE(and_bm.Contains(id_a));
    EXPECT_TRUE (and_bm.Contains(id_b));
    EXPECT_FALSE(and_bm.Contains(id_c));

    // OR("climbing", "jazz") → A and C
    auto or_bm = idx.QueryOr(kFidBio, {"climbing", "jazz"});
    EXPECT_TRUE (or_bm.Contains(id_a));
    EXPECT_FALSE(or_bm.Contains(id_b));
    EXPECT_TRUE (or_bm.Contains(id_c));
}

// ── Delete removes doc from postings ─────────────────────────────────────────

TEST_F(InvertedIndexTest, DeleteRemovesFromPostings) {
    InvertedIndex idx(&alloc, &schema);

    Doc d = idx.NewDoc();
    d.put_int64(kFidUserId, 5);
    d.put_string(kFidBio, "tennis swimming");
    idx.Put(&d);
    uint32_t did = d.doc_id();

    // Confirm indexed
    yikv::container::Bitmap bm(&alloc, 0);
    ASSERT_TRUE(idx.Query(kFidBio, "tennis", &bm));
    EXPECT_TRUE(bm.Contains(did));

    // Delete
    EXPECT_TRUE(idx.Delete("5"));

    // No longer findable via primary key
    Doc out;
    EXPECT_FALSE(idx.Get("5", &out));

    // No longer in posting
    yikv::container::Bitmap bm2(&alloc, 0);
    EXPECT_FALSE(idx.Query(kFidBio, "tennis", &bm2));
}

// ── Update (Put with same pk) re-indexes ─────────────────────────────────────

TEST_F(InvertedIndexTest, UpdateReindexes) {
    InvertedIndex idx(&alloc, &schema);

    Doc d1 = idx.NewDoc();
    d1.put_int64(kFidUserId, 7);
    d1.put_string(kFidBio, "football");
    idx.Put(&d1);
    uint32_t id1 = d1.doc_id();

    // Replace with new doc (same pk)
    Doc d2 = idx.NewDoc();
    d2.put_int64(kFidUserId, 7);
    d2.put_string(kFidBio, "basketball");
    idx.Put(&d2);
    uint32_t id2 = d2.doc_id();

    // Old term "football" should no longer reference old doc
    yikv::container::Bitmap bm_fb(&alloc, 0);
    bool has_fb = idx.Query(kFidBio, "football", &bm_fb);
    EXPECT_TRUE(!has_fb || !bm_fb.Contains(id1));

    // New term "basketball" should reference new doc
    yikv::container::Bitmap bm_bb(&alloc, 0);
    ASSERT_TRUE(idx.Query(kFidBio, "basketball", &bm_bb));
    EXPECT_TRUE(bm_bb.Contains(id2));
}

// ── Recovery from arena offsets ───────────────────────────────────────────────

TEST_F(InvertedIndexTest, RecoverFromOffsets) {
    uint64_t idx_hdr  = 0;
    uint64_t docs_off = 0;
    uint64_t post_off = 0;
    uint32_t doc_id   = 0;

    {
        InvertedIndex idx(&alloc, &schema);
        Doc d = idx.NewDoc();
        d.put_int64(kFidUserId, 99);
        d.put_string(kFidBio, "chess backgammon");
        idx.Put(&d);
        doc_id   = d.doc_id();
        idx_hdr  = idx.index_hdr_offset();
        docs_off = idx.docs_root_offset();
        post_off = idx.posting_root_offset();
    }
    {
        InvertedIndex recovered(&alloc, &schema, idx_hdr, docs_off, post_off);

        Doc out;
        ASSERT_TRUE(recovered.Get("99", &out));
        EXPECT_EQ(out.get_int64(kFidUserId), 99);

        yikv::container::Bitmap bm(&alloc, 0);
        ASSERT_TRUE(recovered.Query(kFidBio, "chess", &bm));
        EXPECT_TRUE(bm.Contains(doc_id));

        ASSERT_TRUE(recovered.Query(kFidBio, "backgammon", &bm));
        EXPECT_TRUE(bm.Contains(doc_id));
    }
}

// ── Query for term not present returns false ──────────────────────────────────

TEST_F(InvertedIndexTest, QueryMissingTermReturnsFalse) {
    InvertedIndex idx(&alloc, &schema);
    yikv::container::Bitmap bm(&alloc, 0);
    EXPECT_FALSE(idx.Query(kFidBio, "nonexistent", &bm));
}

// ── AND with missing term returns empty bitmap ────────────────────────────────

TEST_F(InvertedIndexTest, AndWithMissingTermReturnsEmpty) {
    InvertedIndex idx(&alloc, &schema);
    Doc d = idx.NewDoc();
    d.put_int64(kFidUserId, 3);
    d.put_string(kFidBio, "alpha beta");
    idx.Put(&d);

    auto result = idx.QueryAnd(kFidBio, {"alpha", "gamma"});
    EXPECT_TRUE(result.IsEmpty());
}
