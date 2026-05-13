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

// Schema with a string-array field, used by the string-array tests below.
// fid layout: tag(string pk)=1, tags(string[])=2.
static const char* kStringArraySchemaJson = R"({
  "table_name": "tag_doc",
  "pk": "tag",
  "fields": [
    {"name": "tag",  "data_type": "string", "is_pk": true,  "field_id": 1},
    {"name": "tags", "data_type": "string", "is_pk": false, "field_id": 2, "is_array": true}
  ]
})";

TEST_F(KVIndexTest, StringArrayPutAndGet) {
    Schema s;
    std::string err;
    ASSERT_TRUE(s.LoadJson(kStringArraySchemaJson, &err)) << err;
    KVIndex idx(&alloc, &s);

    Doc d = idx.NewDoc();
    d.put_string(1, "k1");
    std::string_view parts[] = {"alpha", "beta", "gamma"};
    d.array_put_string(2, parts, 3);
    idx.Put(&d);

    Doc out;
    ASSERT_TRUE(idx.Get("k1", &out));
    ASSERT_EQ(out.array_size(2), 3u);
    EXPECT_EQ(out.array_get_string(2, 0), "alpha");
    EXPECT_EQ(out.array_get_string(2, 1), "beta");
    EXPECT_EQ(out.array_get_string(2, 2), "gamma");
}

TEST_F(KVIndexTest, StringArrayAppendOnEmpty) {
    // array_append_string against a slot with b == 0 must initialize the
    // buffer (delegates to array_put_string internally).
    Schema s;
    std::string err;
    ASSERT_TRUE(s.LoadJson(kStringArraySchemaJson, &err)) << err;
    KVIndex idx(&alloc, &s);

    Doc d = idx.NewDoc();
    d.put_string(1, "kE");
    d.array_append_string(2, "solo");
    idx.Put(&d);

    Doc out;
    ASSERT_TRUE(idx.Get("kE", &out));
    ASSERT_EQ(out.array_size(2), 1u);
    EXPECT_EQ(out.array_get_string(2, 0), "solo");
}

TEST_F(KVIndexTest, StringArrayAppendManyAcrossGrow) {
    // Append enough elements (and total bytes) to force the geometric grow
    // path in array_append_strings at least a few times, then verify every
    // element reads back correctly in order. Element strings vary in length
    // to also exercise the bytes_used / cap_bytes accounting.
    Schema s;
    std::string err;
    ASSERT_TRUE(s.LoadJson(kStringArraySchemaJson, &err)) << err;
    KVIndex idx(&alloc, &s);

    Doc d = idx.NewDoc();
    d.put_string(1, "kG");
    constexpr int kN = 200;
    std::vector<std::string> expected;
    expected.reserve(kN);
    for (int i = 0; i < kN; ++i) {
        // Varying-length payload: every 13th element is a 40-byte string.
        std::string v = "v" + std::to_string(i);
        if (i % 13 == 0) v.append(40, 'X');
        expected.push_back(v);
        d.array_append_string(2, expected.back());
    }
    idx.Put(&d);

    Doc out;
    ASSERT_TRUE(idx.Get("kG", &out));
    ASSERT_EQ(out.array_size(2), static_cast<uint32_t>(kN));
    for (int i = 0; i < kN; ++i) {
        EXPECT_EQ(out.array_get_string(2, i), expected[i])
            << "mismatch at index " << i;
    }
}

TEST_F(KVIndexTest, StringArrayBatchAppend) {
    // Mix: put a few, single-append a few, batch-append a few, all in order.
    Schema s;
    std::string err;
    ASSERT_TRUE(s.LoadJson(kStringArraySchemaJson, &err)) << err;
    KVIndex idx(&alloc, &s);

    Doc d = idx.NewDoc();
    d.put_string(1, "kB");

    std::string_view seed[] = {"s0", "s1"};
    d.array_put_string(2, seed, 2);
    d.array_append_string(2, "a0");
    d.array_append_string(2, "a1");
    std::string_view batch[] = {"b0", "b1", "b2", "b3"};
    d.array_append_strings(2, batch, 4);
    idx.Put(&d);

    Doc out;
    ASSERT_TRUE(idx.Get("kB", &out));
    ASSERT_EQ(out.array_size(2), 8u);
    EXPECT_EQ(out.array_get_string(2, 0), "s0");
    EXPECT_EQ(out.array_get_string(2, 1), "s1");
    EXPECT_EQ(out.array_get_string(2, 2), "a0");
    EXPECT_EQ(out.array_get_string(2, 3), "a1");
    EXPECT_EQ(out.array_get_string(2, 4), "b0");
    EXPECT_EQ(out.array_get_string(2, 5), "b1");
    EXPECT_EQ(out.array_get_string(2, 6), "b2");
    EXPECT_EQ(out.array_get_string(2, 7), "b3");
}

TEST_F(KVIndexTest, StringArrayEmptyAndBoundary) {
    Schema s;
    std::string err;
    ASSERT_TRUE(s.LoadJson(kStringArraySchemaJson, &err)) << err;
    KVIndex idx(&alloc, &s);

    Doc d = idx.NewDoc();
    d.put_string(1, "kZ");

    // Empty parts are legal: lens[i] == 0 with no bytes contributed.
    d.array_append_string(2, "");
    d.array_append_string(2, "non-empty");
    d.array_append_string(2, "");
    idx.Put(&d);

    Doc out;
    ASSERT_TRUE(idx.Get("kZ", &out));
    ASSERT_EQ(out.array_size(2), 3u);
    EXPECT_EQ(out.array_get_string(2, 0), "");
    EXPECT_EQ(out.array_get_string(2, 1), "non-empty");
    EXPECT_EQ(out.array_get_string(2, 2), "");
    // Out-of-bounds returns an empty view rather than crashing.
    EXPECT_EQ(out.array_get_string(2, 99), "");
}

// ---------------------------------------------------------------------------
// Batch-append and full-array view tests for numeric/string array types.
// ---------------------------------------------------------------------------

// Schema with one of each numeric array type + a string array, used by the
// batch-append tests below. fids: pk=1, i32=2, i64=3, f32=4, f64=5, str=6.
static const char* kBatchArraySchemaJson = R"({
  "table_name": "batch_doc",
  "pk": "k",
  "fields": [
    {"name": "k",   "data_type": "string", "is_pk": true,  "field_id": 1},
    {"name": "i32", "data_type": "int32",  "is_pk": false, "field_id": 2, "is_array": true},
    {"name": "i64", "data_type": "int64",  "is_pk": false, "field_id": 3, "is_array": true},
    {"name": "f32", "data_type": "float",  "is_pk": false, "field_id": 4, "is_array": true},
    {"name": "f64", "data_type": "double", "is_pk": false, "field_id": 5, "is_array": true},
    {"name": "str", "data_type": "string", "is_pk": false, "field_id": 6, "is_array": true}
  ]
})";

TEST_F(KVIndexTest, ArrayAppendBatchNumeric) {
    Schema s;
    std::string err;
    ASSERT_TRUE(s.LoadJson(kBatchArraySchemaJson, &err)) << err;
    KVIndex idx(&alloc, &s);

    Doc d = idx.NewDoc();
    d.put_string(1, "kBN");

    // Append in two batches per type to exercise both the empty-slot delegate
    // path (first call → array_put_impl) and the fast-path in-place batch
    // (second call uses the reserved 1.2x slack).
    const int32_t i32_a[] = {1, 2, 3};
    const int32_t i32_b[] = {4, 5};
    d.array_append_int32s(2, i32_a, 3);
    d.array_append_int32s(2, i32_b, 2);

    const int64_t i64_a[] = {10, 20};
    const int64_t i64_b[] = {30, 40, 50};
    d.array_append_int64s(3, i64_a, 2);
    d.array_append_int64s(3, i64_b, 3);

    const float f32_a[] = {1.5f, 2.5f};
    const float f32_b[] = {3.5f};
    d.array_append_floats(4, f32_a, 2);
    d.array_append_floats(4, f32_b, 1);

    const double f64_a[] = {1.25, 2.25, 3.25};
    const double f64_b[] = {4.25, 5.25};
    d.array_append_doubles(5, f64_a, 3);
    d.array_append_doubles(5, f64_b, 2);

    idx.Put(&d);

    Doc out;
    ASSERT_TRUE(idx.Get("kBN", &out));

    auto v32 = out.array_view_int32(2);
    ASSERT_EQ(v32.second, 5u);
    for (int i = 0; i < 5; ++i) EXPECT_EQ(v32.first[i], i + 1);

    auto v64 = out.array_view_int64(3);
    ASSERT_EQ(v64.second, 5u);
    const int64_t want64[] = {10, 20, 30, 40, 50};
    for (int i = 0; i < 5; ++i) EXPECT_EQ(v64.first[i], want64[i]);

    auto vf = out.array_view_float(4);
    ASSERT_EQ(vf.second, 3u);
    EXPECT_FLOAT_EQ(vf.first[0], 1.5f);
    EXPECT_FLOAT_EQ(vf.first[1], 2.5f);
    EXPECT_FLOAT_EQ(vf.first[2], 3.5f);

    auto vd = out.array_view_double(5);
    ASSERT_EQ(vd.second, 5u);
    const double wantD[] = {1.25, 2.25, 3.25, 4.25, 5.25};
    for (int i = 0; i < 5; ++i) EXPECT_DOUBLE_EQ(vd.first[i], wantD[i]);
}

TEST_F(KVIndexTest, ArrayAppendBatchNumericAcrossGrow) {
    // Append a large enough total to force the slow (grow) path multiple
    // times, then verify the contents read back in order.
    Schema s;
    std::string err;
    ASSERT_TRUE(s.LoadJson(kBatchArraySchemaJson, &err)) << err;
    KVIndex idx(&alloc, &s);

    Doc d = idx.NewDoc();
    d.put_string(1, "kBG");

    constexpr int kBatch = 50;
    constexpr int kRounds = 7;  // 350 elements total > initial 1.2x slack
    std::vector<int64_t> chunk(kBatch);
    int64_t total = 0;
    for (int r = 0; r < kRounds; ++r) {
        for (int i = 0; i < kBatch; ++i) chunk[i] = total++;
        d.array_append_int64s(3, chunk.data(), static_cast<uint32_t>(kBatch));
    }
    idx.Put(&d);

    Doc out;
    ASSERT_TRUE(idx.Get("kBG", &out));
    auto v = out.array_view_int64(3);
    ASSERT_EQ(v.second, static_cast<uint32_t>(kBatch * kRounds));
    for (int i = 0; i < kBatch * kRounds; ++i) {
        EXPECT_EQ(v.first[i], i) << "mismatch at " << i;
    }
}

TEST_F(KVIndexTest, ArrayAppendBatchNumericZeroCount) {
    // Batch append of 0 elements must be a no-op (and not crash on nullptr).
    Schema s;
    std::string err;
    ASSERT_TRUE(s.LoadJson(kBatchArraySchemaJson, &err)) << err;
    KVIndex idx(&alloc, &s);

    Doc d = idx.NewDoc();
    d.put_string(1, "kZ");
    d.array_append_int32s(2, nullptr, 0);
    d.array_append_int32(2, 7);
    d.array_append_int32s(2, nullptr, 0);
    idx.Put(&d);

    Doc out;
    ASSERT_TRUE(idx.Get("kZ", &out));
    auto v = out.array_view_int32(2);
    ASSERT_EQ(v.second, 1u);
    EXPECT_EQ(v.first[0], 7);
}

TEST_F(KVIndexTest, ArrayViewStringReturnsAllElementsInOrder) {
    Schema s;
    std::string err;
    ASSERT_TRUE(s.LoadJson(kBatchArraySchemaJson, &err)) << err;
    KVIndex idx(&alloc, &s);

    Doc d = idx.NewDoc();
    d.put_string(1, "kV");

    // Mix put + single append + batch append to exercise the layout
    // after multiple grow paths.
    std::string_view seed[] = {"alpha", "beta"};
    d.array_put_string(6, seed, 2);
    d.array_append_string(6, "gamma");
    std::string_view rest[] = {"delta", "", "epsilon"};
    d.array_append_strings(6, rest, 3);
    idx.Put(&d);

    Doc out;
    ASSERT_TRUE(idx.Get("kV", &out));

    // Value-returning overload.
    auto views = out.array_view_string(6);
    ASSERT_EQ(views.size(), 6u);
    EXPECT_EQ(views[0], "alpha");
    EXPECT_EQ(views[1], "beta");
    EXPECT_EQ(views[2], "gamma");
    EXPECT_EQ(views[3], "delta");
    EXPECT_EQ(views[4], "");
    EXPECT_EQ(views[5], "epsilon");

    // Out-param overload appends without clearing — prepopulate a sentinel
    // to verify that contract.
    std::vector<std::string_view> sink;
    sink.emplace_back("sentinel");
    out.array_view_string(6, &sink);
    ASSERT_EQ(sink.size(), 7u);
    EXPECT_EQ(sink[0], "sentinel");
    EXPECT_EQ(sink[1], "alpha");
    EXPECT_EQ(sink[6], "epsilon");
}

TEST_F(KVIndexTest, ArrayViewStringEmptySlot) {
    Schema s;
    std::string err;
    ASSERT_TRUE(s.LoadJson(kBatchArraySchemaJson, &err)) << err;
    KVIndex idx(&alloc, &s);

    Doc d = idx.NewDoc();
    d.put_string(1, "kE");
    // No writes to fid=6 (the string array slot).
    idx.Put(&d);

    Doc out;
    ASSERT_TRUE(idx.Get("kE", &out));
    auto views = out.array_view_string(6);
    EXPECT_TRUE(views.empty());

    std::vector<std::string_view> sink;
    out.array_view_string(6, &sink);
    EXPECT_TRUE(sink.empty());
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
