// Unit tests for json_stream_ingest: merge semantics, array append, op dispatch.
// No Kafka / OSS dependency.  Uses FtAllocator + KVIndex in-memory.

#include "stream/json_stream_ingest.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "alloc/ft_allocator.h"
#include "index/kv_index.h"
#include "schema/schema.h"

using yikv::alloc::AllocatorOptions;
using yikv::alloc::FtAllocator;
using yikv::index::Doc;
using yikv::index::KVIndex;
using yikv::schema::Schema;
using yikv_server::stream::ApplyStreamJsonObject;
using yikv_server::stream::LogFn;

// Schema:
//   user_id    int64   pk  fid=1
//   age        int32       fid=2
//   name       string      fid=3
//   tags       string[]    fid=4
//   clicks     int64[]     fid=5
//   score      float64     fid=6
static const char* kSchema = R"({
  "table_name": "user",
  "pk": "user_id",
  "fields": [
    {"name":"user_id","data_type":"int64","is_pk":true, "field_id":1},
    {"name":"age",    "data_type":"int32",              "field_id":2},
    {"name":"name",   "data_type":"string",             "field_id":3},
    {"name":"tags",   "data_type":"string","is_array":true,"field_id":4},
    {"name":"clicks", "data_type":"int64", "is_array":true,"field_id":5},
    {"name":"score",  "data_type":"float64",            "field_id":6}
  ]
})";

static constexpr uint32_t kFidUserId = 1;
static constexpr uint32_t kFidAge    = 2;
static constexpr uint32_t kFidName   = 3;
static constexpr uint32_t kFidTags   = 4;
static constexpr uint32_t kFidClicks = 5;
static constexpr uint32_t kFidScore  = 6;

// ─── fixture ──────────────────────────────────────────────────────────────────

class IngestTest : public ::testing::Test {
protected:
    void SetUp() override {
        AllocatorOptions opts;
        opts.arena_size = 64ULL * 1024 * 1024;
        alloc_.Open(opts);
        std::string err;
        ASSERT_TRUE(schema_.LoadJson(kSchema, &err)) << err;
    }

    KVIndex MakeIndex() { return KVIndex(&alloc_, &schema_); }

    // Collect error strings produced by ApplyStreamJsonObject.
    static LogFn Capture(std::vector<std::string>* out) {
        return [out](std::string_view s) { out->emplace_back(s); };
    }
    static LogFn Sink() { return [](std::string_view) {}; }

    FtAllocator alloc_;
    Schema      schema_;
};

// ─── basic insert / get ───────────────────────────────────────────────────────

TEST_F(IngestTest, UpsertNewRow) {
    auto idx = MakeIndex();
    auto j = nlohmann::json::parse(
        R"({"user_id":1,"age":30,"name":"Alice","score":9.5})");
    ASSERT_TRUE(ApplyStreamJsonObject(&idx, &schema_, j, Sink()));

    Doc out;
    ASSERT_TRUE(idx.Get("1", &out));
    EXPECT_EQ(out.get_int64(kFidUserId), 1);
    EXPECT_EQ(out.get_int32(kFidAge),    30);
    EXPECT_EQ(out.get_string(kFidName),  "Alice");
    EXPECT_DOUBLE_EQ(out.get_double(kFidScore), 9.5);
}

// ─── core: missing fields leave existing values unchanged ─────────────────────

TEST_F(IngestTest, MissingFieldsNotOverwritten) {
    auto idx = MakeIndex();

    // First message: set all three scalar fields.
    auto j1 = nlohmann::json::parse(
        R"({"user_id":42,"age":25,"name":"Bob","score":7.0})");
    ASSERT_TRUE(ApplyStreamJsonObject(&idx, &schema_, j1, Sink()));

    // Second message: only update age; name and score must stay.
    auto j2 = nlohmann::json::parse(R"({"user_id":42,"age":26})");
    ASSERT_TRUE(ApplyStreamJsonObject(&idx, &schema_, j2, Sink()));

    Doc out;
    ASSERT_TRUE(idx.Get("42", &out));
    EXPECT_EQ(out.get_int32(kFidAge),   26);           // updated
    EXPECT_EQ(out.get_string(kFidName), "Bob");         // unchanged
    EXPECT_DOUBLE_EQ(out.get_double(kFidScore), 7.0);  // unchanged
}

TEST_F(IngestTest, OnlyPkInMessageLeavesEverythingElse) {
    auto idx = MakeIndex();

    auto j1 = nlohmann::json::parse(R"({"user_id":99,"age":40,"name":"Carol"})");
    ASSERT_TRUE(ApplyStreamJsonObject(&idx, &schema_, j1, Sink()));

    // Message with only the pk (no other fields).
    auto j2 = nlohmann::json::parse(R"({"user_id":99})");
    ASSERT_TRUE(ApplyStreamJsonObject(&idx, &schema_, j2, Sink()));

    Doc out;
    ASSERT_TRUE(idx.Get("99", &out));
    EXPECT_EQ(out.get_int32(kFidAge),   40);
    EXPECT_EQ(out.get_string(kFidName), "Carol");
}

// ─── array: append semantics ──────────────────────────────────────────────────

TEST_F(IngestTest, ArrayAppendsAcrossMessages) {
    auto idx = MakeIndex();

    auto j1 = nlohmann::json::parse(R"({"user_id":10,"clicks":[100,200]})");
    ASSERT_TRUE(ApplyStreamJsonObject(&idx, &schema_, j1, Sink()));

    auto j2 = nlohmann::json::parse(R"({"user_id":10,"clicks":[300]})");
    ASSERT_TRUE(ApplyStreamJsonObject(&idx, &schema_, j2, Sink()));

    Doc out;
    ASSERT_TRUE(idx.Get("10", &out));
    ASSERT_EQ(out.array_size(kFidClicks), 3u);
    EXPECT_EQ(out.array_get_int64(kFidClicks, 0), 100);
    EXPECT_EQ(out.array_get_int64(kFidClicks, 1), 200);
    EXPECT_EQ(out.array_get_int64(kFidClicks, 2), 300);  // appended, not replaced
}

TEST_F(IngestTest, StringArrayAppend) {
    auto idx = MakeIndex();

    auto j1 = nlohmann::json::parse(R"({"user_id":11,"tags":["sports","tech"]})");
    ASSERT_TRUE(ApplyStreamJsonObject(&idx, &schema_, j1, Sink()));

    auto j2 = nlohmann::json::parse(R"({"user_id":11,"tags":["music"]})");
    ASSERT_TRUE(ApplyStreamJsonObject(&idx, &schema_, j2, Sink()));

    Doc out;
    ASSERT_TRUE(idx.Get("11", &out));
    ASSERT_EQ(out.array_size(kFidTags), 3u);
    EXPECT_EQ(out.array_get_string(kFidTags, 0), "sports");
    EXPECT_EQ(out.array_get_string(kFidTags, 1), "tech");
    EXPECT_EQ(out.array_get_string(kFidTags, 2), "music");
}

TEST_F(IngestTest, ArrayAppendDoesNotAffectOtherScalarFields) {
    auto idx = MakeIndex();

    auto j1 = nlohmann::json::parse(R"({"user_id":12,"age":18,"clicks":[1,2]})");
    ASSERT_TRUE(ApplyStreamJsonObject(&idx, &schema_, j1, Sink()));

    // Append to clicks only; age must stay.
    auto j2 = nlohmann::json::parse(R"({"user_id":12,"clicks":[3]})");
    ASSERT_TRUE(ApplyStreamJsonObject(&idx, &schema_, j2, Sink()));

    Doc out;
    ASSERT_TRUE(idx.Get("12", &out));
    EXPECT_EQ(out.get_int32(kFidAge),       18);   // untouched
    EXPECT_EQ(out.array_size(kFidClicks),   3u);
}

// ─── unknown / irrelevant fields ──────────────────────────────────────────────

TEST_F(IngestTest, UnknownFieldSilentlyIgnored) {
    auto idx = MakeIndex();
    auto j = nlohmann::json::parse(
        R"({"user_id":20,"age":22,"_irrelevant":"x","not_a_column":999})");
    ASSERT_TRUE(ApplyStreamJsonObject(&idx, &schema_, j, Sink()));

    Doc out;
    ASSERT_TRUE(idx.Get("20", &out));
    EXPECT_EQ(out.get_int32(kFidAge), 22);
}

TEST_F(IngestTest, KafkaMetaFieldsSkipped) {
    // _op and _ts must never be treated as data columns.
    auto idx = MakeIndex();
    auto j = nlohmann::json::parse(
        R"({"_op":"UPSERT","_ts":1700000000000,"user_id":21,"age":35})");
    ASSERT_TRUE(ApplyStreamJsonObject(&idx, &schema_, j, Sink()));

    Doc out;
    ASSERT_TRUE(idx.Get("21", &out));
    EXPECT_EQ(out.get_int32(kFidAge), 35);
}

// ─── null value handling ──────────────────────────────────────────────────────

TEST_F(IngestTest, NullValueForExistingFieldDoesNotOverwrite) {
    // A null in the JSON payload means "not provided" → must not clear the field.
    auto idx = MakeIndex();

    auto j1 = nlohmann::json::parse(R"({"user_id":30,"age":50,"name":"Dave"})");
    ASSERT_TRUE(ApplyStreamJsonObject(&idx, &schema_, j1, Sink()));

    auto j2 = nlohmann::json::parse(R"({"user_id":30,"age":null,"name":null})");
    ASSERT_TRUE(ApplyStreamJsonObject(&idx, &schema_, j2, Sink()));

    Doc out;
    ASSERT_TRUE(idx.Get("30", &out));
    EXPECT_EQ(out.get_int32(kFidAge),   50);     // must stay
    EXPECT_EQ(out.get_string(kFidName), "Dave");  // must stay
}

// ─── op dispatch ─────────────────────────────────────────────────────────────

TEST_F(IngestTest, NoOpFieldDefaultsToUpsertMerge) {
    auto idx = MakeIndex();
    auto j = nlohmann::json::parse(R"({"user_id":40,"age":20})");
    ASSERT_TRUE(ApplyStreamJsonObject(&idx, &schema_, j, Sink()));
    Doc out;
    ASSERT_TRUE(idx.Get("40", &out));
    EXPECT_EQ(out.get_int32(kFidAge), 20);
}

TEST_F(IngestTest, OpFieldCaseInsensitive) {
    auto idx = MakeIndex();
    for (const char* op : {"UPSERT", "upsert", "Upsert", "UPSERT"}) {
        std::string raw =
            R"({"_op":")" + std::string(op) + R"(","user_id":50,"age":1})";
        ASSERT_TRUE(ApplyStreamJsonObject(&idx, &schema_,
                                          nlohmann::json::parse(raw), Sink()))
            << "op=" << op;
    }
    Doc out;
    ASSERT_TRUE(idx.Get("50", &out));
    EXPECT_EQ(out.get_int32(kFidAge), 1);
}

TEST_F(IngestTest, DeleteRemovesRow) {
    auto idx = MakeIndex();
    auto j1 = nlohmann::json::parse(R"({"user_id":60,"age":99})");
    ASSERT_TRUE(ApplyStreamJsonObject(&idx, &schema_, j1, Sink()));
    ASSERT_EQ(idx.Size(), 1u);

    auto jdel = nlohmann::json::parse(R"({"_op":"DELETE","user_id":60})");
    ASSERT_TRUE(ApplyStreamJsonObject(&idx, &schema_, jdel, Sink()));
    EXPECT_EQ(idx.Size(), 0u);

    Doc out;
    EXPECT_FALSE(idx.Get("60", &out));
}

TEST_F(IngestTest, DeleteCaseInsensitive) {
    auto idx = MakeIndex();
    auto j1 = nlohmann::json::parse(R"({"user_id":61,"age":1})");
    ApplyStreamJsonObject(&idx, &schema_, j1, Sink());

    auto jdel = nlohmann::json::parse(R"({"_op":"delete","user_id":61})");
    ASSERT_TRUE(ApplyStreamJsonObject(&idx, &schema_, jdel, Sink()));
    EXPECT_EQ(idx.Size(), 0u);
}

TEST_F(IngestTest, InsertSucceedsOnNewPk) {
    auto idx = MakeIndex();
    auto j = nlohmann::json::parse(R"({"_op":"INSERT","user_id":70,"age":5})");
    ASSERT_TRUE(ApplyStreamJsonObject(&idx, &schema_, j, Sink()));
    EXPECT_EQ(idx.Size(), 1u);
}

TEST_F(IngestTest, InsertFailsOnDuplicatePk) {
    auto idx = MakeIndex();
    auto j1 = nlohmann::json::parse(R"({"user_id":71,"age":5})");
    ApplyStreamJsonObject(&idx, &schema_, j1, Sink());

    std::vector<std::string> errs;
    auto j2 = nlohmann::json::parse(R"({"_op":"INSERT","user_id":71,"age":9})");
    EXPECT_FALSE(ApplyStreamJsonObject(&idx, &schema_, j2, Capture(&errs)));
    EXPECT_FALSE(errs.empty());

    // Original value must be intact.
    Doc out;
    ASSERT_TRUE(idx.Get("71", &out));
    EXPECT_EQ(out.get_int32(kFidAge), 5);
}

TEST_F(IngestTest, UnknownOpReturnsError) {
    auto idx = MakeIndex();
    std::vector<std::string> errs;
    auto j = nlohmann::json::parse(R"({"_op":"REPLACE","user_id":80,"age":1})");
    EXPECT_FALSE(ApplyStreamJsonObject(&idx, &schema_, j, Capture(&errs)));
    EXPECT_FALSE(errs.empty());
}

// ─── multi-message sequence ───────────────────────────────────────────────────

TEST_F(IngestTest, ThreeMessagesAccumulate) {
    auto idx = MakeIndex();

    // Message 1: set baseline.
    ApplyStreamJsonObject(&idx, &schema_,
        nlohmann::json::parse(R"({"user_id":90,"age":10,"name":"Eve","clicks":[1]})"),
        Sink());

    // Message 2: update age, append click — name must stay.
    ApplyStreamJsonObject(&idx, &schema_,
        nlohmann::json::parse(R"({"user_id":90,"age":11,"clicks":[2]})"),
        Sink());

    // Message 3: append another click — age and name must stay.
    ApplyStreamJsonObject(&idx, &schema_,
        nlohmann::json::parse(R"({"user_id":90,"clicks":[3,4]})"),
        Sink());

    Doc out;
    ASSERT_TRUE(idx.Get("90", &out));
    EXPECT_EQ(out.get_int32(kFidAge),   11);
    EXPECT_EQ(out.get_string(kFidName), "Eve");
    ASSERT_EQ(out.array_size(kFidClicks), 4u);
    EXPECT_EQ(out.array_get_int64(kFidClicks, 0), 1);
    EXPECT_EQ(out.array_get_int64(kFidClicks, 1), 2);
    EXPECT_EQ(out.array_get_int64(kFidClicks, 2), 3);
    EXPECT_EQ(out.array_get_int64(kFidClicks, 3), 4);
}

// ─── multiple independent rows ────────────────────────────────────────────────

TEST_F(IngestTest, DifferentRowsDoNotInterfere) {
    auto idx = MakeIndex();

    ApplyStreamJsonObject(&idx, &schema_,
        nlohmann::json::parse(R"({"user_id":1,"age":10})"), Sink());
    ApplyStreamJsonObject(&idx, &schema_,
        nlohmann::json::parse(R"({"user_id":2,"age":20})"), Sink());
    // Patch row 1 only.
    ApplyStreamJsonObject(&idx, &schema_,
        nlohmann::json::parse(R"({"user_id":1,"name":"X"})"), Sink());

    Doc out2;
    ASSERT_TRUE(idx.Get("2", &out2));
    EXPECT_EQ(out2.get_int32(kFidAge),   20);
    EXPECT_EQ(out2.get_string(kFidName), "");   // never set
}

// ─── missing pk ───────────────────────────────────────────────────────────────

TEST_F(IngestTest, MissingPkReturnsError) {
    auto idx = MakeIndex();
    std::vector<std::string> errs;
    auto j = nlohmann::json::parse(R"({"age":30})");  // no user_id
    EXPECT_FALSE(ApplyStreamJsonObject(&idx, &schema_, j, Capture(&errs)));
    EXPECT_FALSE(errs.empty());
    EXPECT_EQ(idx.Size(), 0u);
}

TEST_F(IngestTest, NullPkReturnsError) {
    auto idx = MakeIndex();
    std::vector<std::string> errs;
    auto j = nlohmann::json::parse(R"({"user_id":null,"age":5})");
    EXPECT_FALSE(ApplyStreamJsonObject(&idx, &schema_, j, Capture(&errs)));
    EXPECT_EQ(idx.Size(), 0u);
}
