#include "src/index/kv_index.h"
#include "src/alloc/ft_allocator.h"
#include "src/schema/schema.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using yikv::alloc::AllocatorOptions;
using yikv::alloc::FtAllocator;
using yikv::index::Doc;
using yikv::index::KVIndex;
using yikv::schema::Schema;

// Schema: user_id(int64, pk, fid=1), age(int32, fid=2), name(string, fid=3),
//         clk_list(int64[], fid=4)
static const char* kTestSchemaJson = R"({
  "table_name": "user",
  "pk": "user_id",
  "fields": [
    {"name": "user_id", "data_type": "int64", "is_pk": true,  "is_index": false, "field_id": 1},
    {"name": "age",     "data_type": "int32", "is_pk": false, "is_index": false, "field_id": 2},
    {"name": "name",    "data_type": "string","is_pk": false, "is_index": false, "field_id": 3},
    {"name": "clk_list","data_type": "int64", "is_pk": false, "is_index": false, "field_id": 4,
     "is_array": true}
  ]
})";

static constexpr uint32_t kFidUserId  = 1;
static constexpr uint32_t kFidAge     = 2;
static constexpr uint32_t kFidName    = 3;
static constexpr uint32_t kFidClkList = 4;

static AllocatorOptions AnonArena(std::size_t size = 64 * 1024 * 1024) {
    AllocatorOptions opts;
    opts.arena_size = size;
    return opts;
}

class KVIndexTest : public ::testing::Test {
protected:
    void SetUp() override {
        alloc.Open(AnonArena());
        std::string err;
        ASSERT_TRUE(schema.LoadJson(kTestSchemaJson, &err)) << err;
    }

    FtAllocator alloc;
    Schema      schema;
};

TEST_F(KVIndexTest, PutAndGet) {
    KVIndex idx(&alloc, &schema);

    Doc d = idx.NewDoc();
    d.put_int64(kFidUserId, 42);
    d.put_int32(kFidAge,    25);
    d.put_string(kFidName,  "Alice");
    idx.Put(&d);

    Doc out;
    ASSERT_TRUE(idx.Get("42", &out));
    EXPECT_EQ(out.get_int64(kFidUserId), 42);
    EXPECT_EQ(out.get_int32(kFidAge),    25);
    EXPECT_EQ(out.get_string(kFidName),  "Alice");
}

TEST_F(KVIndexTest, ArrayViewEmpty) {
    KVIndex idx(&alloc, &schema);
    Doc d = idx.NewDoc();
    d.put_int64(kFidUserId, 1);
    idx.Put(&d);
    Doc out;
    ASSERT_TRUE(idx.Get("1", &out));
    auto v = out.array_view_int64(kFidClkList);
    EXPECT_EQ(v.first, nullptr);
    EXPECT_EQ(v.second, 0u);
}

TEST_F(KVIndexTest, GetMissingReturnsFalse) {
    KVIndex idx(&alloc, &schema);
    Doc out;
    EXPECT_FALSE(idx.Get("99", &out));
}

TEST_F(KVIndexTest, OverwriteUpdatesValue) {
    KVIndex idx(&alloc, &schema);

    Doc d1 = idx.NewDoc();
    d1.put_int64(kFidUserId, 7);
    d1.put_int32(kFidAge,    20);
    idx.Put(&d1);

    Doc d2 = idx.NewDoc();
    d2.put_int64(kFidUserId, 7);
    d2.put_int32(kFidAge,    30);
    idx.Upsert(&d2);

    EXPECT_EQ(idx.Size(), 1u);  // still one key "7"

    Doc out;
    ASSERT_TRUE(idx.Get("7", &out));
    // The HashMap points to d2's slot
    EXPECT_EQ(out.get_int64(kFidUserId), 7);
    EXPECT_EQ(out.get_int32(kFidAge),    30);
}

TEST_F(KVIndexTest, Delete) {
    KVIndex idx(&alloc, &schema);

    Doc d = idx.NewDoc();
    d.put_int64(kFidUserId, 5);
    idx.Put(&d);

    EXPECT_TRUE(idx.Delete("5"));
    EXPECT_EQ(idx.Size(), 0u);

    Doc out;
    EXPECT_FALSE(idx.Get("5", &out));

    // Deleting again returns false
    EXPECT_FALSE(idx.Delete("5"));
}

TEST_F(KVIndexTest, BatchPutAndBatchGet) {
    KVIndex idx(&alloc, &schema);

    Doc d1 = idx.NewDoc();
    d1.put_int64(kFidUserId, 10);
    d1.put_int32(kFidAge, 18);

    Doc d2 = idx.NewDoc();
    d2.put_int64(kFidUserId, 20);
    d2.put_int32(kFidAge, 28);

    std::vector<Doc*> batch = {&d1, &d2};
    idx.BatchPut(batch);
    EXPECT_EQ(idx.Size(), 2u);

    std::vector<Doc> results;
    idx.BatchGet({"10", "20", "99"}, &results);
    ASSERT_EQ(results.size(), 2u);
    EXPECT_EQ(results[0].get_int32(kFidAge), 18);
    EXPECT_EQ(results[1].get_int32(kFidAge), 28);
}

TEST_F(KVIndexTest, ArrayFieldAppendAndRecover) {
    KVIndex idx(&alloc, &schema);

    Doc d = idx.NewDoc();
    d.put_int64(kFidUserId, 99);
    d.array_append_int64(kFidClkList, 100);
    d.array_append_int64(kFidClkList, 200);
    d.array_append_int64(kFidClkList, 300);
    idx.Put(&d);

    Doc out;
    ASSERT_TRUE(idx.Get("99", &out));
    ASSERT_EQ(out.array_size(kFidClkList), 3u);
    EXPECT_EQ(out.array_get_int64(kFidClkList, 0), 100);
    EXPECT_EQ(out.array_get_int64(kFidClkList, 1), 200);
    EXPECT_EQ(out.array_get_int64(kFidClkList, 2), 300);

    auto [p, n] = out.array_view_int64(kFidClkList);
    ASSERT_EQ(n, 3u);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(p[0], 100);
    EXPECT_EQ(p[1], 200);
    EXPECT_EQ(p[2], 300);
}

TEST_F(KVIndexTest, RecoverFromOffsets) {
    uint64_t idx_hdr = 0;
    uint64_t docs_hdr = 0;
    {
        KVIndex idx(&alloc, &schema);
        Doc d = idx.NewDoc();
        d.put_int64(kFidUserId, 1);
        d.put_string(kFidName, "Bob");
        idx.Put(&d);
        idx_hdr  = idx.index_hdr_offset();
        docs_hdr = idx.docs_root_offset();
    }
    {
        KVIndex recovered(&alloc, &schema, idx_hdr, docs_hdr);
        Doc out;
        ASSERT_TRUE(recovered.Get("1", &out));
        EXPECT_EQ(out.get_int64(kFidUserId), 1);
        EXPECT_EQ(out.get_string(kFidName), "Bob");
    }
}

// Verify that repeated upserts on the same key do not leak arena memory.
// publish() now auto-reclaims inline; no explicit Reclaim() call needed.
// Uses reclaim_delay_ns=0 so reclamation is immediate (no sleep needed).
TEST_F(KVIndexTest, UpsertReclaimMemoryTrend) {
    FtAllocator fast_alloc;
    AllocatorOptions fast_opts = AnonArena(64 * 1024 * 1024);
    fast_opts.reclaim_delay_ns = 0;
    fast_alloc.Open(fast_opts);
    KVIndex idx(&fast_alloc, &schema);

    constexpr int kRounds = 200;

    // First upsert — establishes the baseline footprint.
    {
        Doc d = idx.NewDoc();
        d.put_int64(kFidUserId, 55);
        d.put_string(kFidName, "baseline");
        d.array_append_int64(kFidClkList, 1);
        d.array_append_int64(kFidClkList, 2);
        idx.Put(&d);
    }
    auto stats_after_first = fast_alloc.GetStats();

    // Upsert the same key kRounds more times — publish() reclaims inline.
    for (int i = 0; i < kRounds; ++i) {
        Doc d = idx.NewDoc();
        d.put_int64(kFidUserId, 55);
        d.put_string(kFidName, "updated");
        d.array_append_int64(kFidClkList, i);
        d.array_append_int64(kFidClkList, i + 1);
        idx.Upsert(&d);
    }

    auto stats_final = fast_alloc.GetStats();

    // After 200 upserts, used_bytes must stay within 3× of the one-row baseline.
    EXPECT_LT(stats_final.used_bytes, stats_after_first.used_bytes * 3)
        << "used_bytes grew from " << stats_after_first.used_bytes
        << " to " << stats_final.used_bytes
        << " after " << kRounds << " upsert cycles — likely a leak";

    // Doc must still be readable with the latest values.
    Doc out;
    ASSERT_TRUE(idx.Get("55", &out));
    EXPECT_EQ(out.get_int64(kFidUserId), 55);
    EXPECT_EQ(out.get_string(kFidName), "updated");
    fast_alloc.Close();
}

TEST_F(KVIndexTest, StringPkExtraction) {
    // Use a schema whose pk is a string field.
    static const char* kStrPkSchema = R"({
      "table_name": "tags",
      "pk": "tag",
      "fields": [
        {"name": "tag",   "data_type": "string", "is_pk": true,  "field_id": 1},
        {"name": "count", "data_type": "int32",  "is_pk": false, "field_id": 2}
      ]
    })";
    Schema s2;
    std::string err;
    ASSERT_TRUE(s2.LoadJson(kStrPkSchema, &err)) << err;

    KVIndex idx(&alloc, &s2);
    Doc d = idx.NewDoc();
    d.put_string(1, "sports");
    d.put_int32(2, 7);
    idx.Put(&d);

    Doc out;
    ASSERT_TRUE(idx.Get("sports", &out));
    EXPECT_EQ(out.get_string(1), "sports");
    EXPECT_EQ(out.get_int32(2), 7);
}
