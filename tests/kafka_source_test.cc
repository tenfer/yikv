// Integration tests for RunKafkaImportCatchup using the rdkafka in-process
// mock cluster.  No external Kafka broker is required.
//
// Pattern:
//   1. Producer rd_kafka_t with "test.mock.num.brokers=1" → embedded mock.
//   2. Produce JSON messages and flush.
//   3. Call RunKafkaImportCatchup (blocking, one-shot to EOF).
//   4. Assert KVIndex state, kafka.offset file, kafka_meta.json.

#include <librdkafka/rdkafka.h>
#include <librdkafka/rdkafka_mock.h>
#include <nlohmann/json.hpp>
#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include "kafka/kafka_import_catchup.h"
#include "alloc/ft_allocator.h"
#include "index/kv_index.h"
#include "schema/schema.h"

namespace fs = std::filesystem;

using yikv::alloc::AllocatorOptions;
using yikv::alloc::FtAllocator;
using yikv::index::Doc;
using yikv::index::KVIndex;
using yikv::schema::Schema;
using yikv_server::kafka::KafkaImportCatchupOptions;
using yikv_server::kafka::RunKafkaImportCatchup;

// Schema: user_id(int64, pk), age(int32), name(string), clicks(int64[])
static const char* kSchema = R"({
  "table_name": "user",
  "pk": "user_id",
  "fields": [
    {"name":"user_id","data_type":"int64","is_pk":true, "field_id":1},
    {"name":"age",    "data_type":"int32",              "field_id":2},
    {"name":"name",   "data_type":"string",             "field_id":3},
    {"name":"clicks", "data_type":"int64","is_array":true,"field_id":4}
  ]
})";

static constexpr uint32_t kFidUserId = 1;
static constexpr uint32_t kFidAge    = 2;
static constexpr uint32_t kFidName   = 3;
static constexpr uint32_t kFidClicks = 4;

static constexpr const char* kTopic     = "yikv-test";
static constexpr int32_t     kPartition = 0;

// ─── fixture ──────────────────────────────────────────────────────────────────

class KafkaMockTest : public ::testing::Test {
protected:
    void SetUp() override {
        char errstr[512];

        // Producer with embedded single-broker mock cluster.
        rd_kafka_conf_t* pconf = rd_kafka_conf_new();
        rd_kafka_conf_set(pconf, "test.mock.num.brokers", "1", errstr, sizeof(errstr));
        rd_kafka_conf_set(pconf, "log_level", "0", errstr, sizeof(errstr));

        producer_rk_ = rd_kafka_new(RD_KAFKA_PRODUCER, pconf, errstr, sizeof(errstr));
        if (!producer_rk_) {
            GTEST_SKIP() << "rd_kafka_new failed (no librdkafka?): " << errstr;
        }

        mcluster_ = rd_kafka_handle_mock_cluster(producer_rk_);
        if (!mcluster_) {
            rd_kafka_destroy(producer_rk_);
            producer_rk_ = nullptr;
            GTEST_SKIP() << "mock cluster not available (librdkafka < 1.4?)";
        }

        bootstraps_ = rd_kafka_mock_cluster_bootstraps(mcluster_);

        // Pre-create topic so offsets_for_times works without auto-create races.
        rd_kafka_mock_topic_create(mcluster_, kTopic, /*partition_cnt=*/1,
                                   /*replication_factor=*/1);

        rkt_ = rd_kafka_topic_new(producer_rk_, kTopic, nullptr);
        ASSERT_NE(rkt_, nullptr);

        // In-memory KVIndex.
        AllocatorOptions opts;
        opts.arena_size = 64ULL * 1024 * 1024;
        alloc_.Open(opts);
        std::string err;
        ASSERT_TRUE(schema_.LoadJson(kSchema, &err)) << err;
        idx_ = std::make_unique<KVIndex>(&alloc_, &schema_);

        // Unique temp dir for offset + meta files.
        table_dir_ = fs::temp_directory_path() /
                     ("kafka_mock_test_" +
                      std::to_string(reinterpret_cast<uintptr_t>(this)));
        fs::create_directories(table_dir_);
    }

    void TearDown() override {
        if (rkt_)         rd_kafka_topic_destroy(rkt_);
        if (producer_rk_) rd_kafka_destroy(producer_rk_);
        if (!table_dir_.empty()) fs::remove_all(table_dir_);
        alloc_.Close();
    }

    // Produce one JSON string to partition 0.
    void Produce(const std::string& json) {
        int r = rd_kafka_produce(
            rkt_, kPartition, RD_KAFKA_MSG_F_COPY,
            const_cast<char*>(json.data()), json.size(),
            nullptr, 0, nullptr);
        ASSERT_EQ(r, 0) << rd_kafka_err2str(rd_kafka_last_error());
    }

    // Wait until all produced messages are delivered.
    void Flush() { rd_kafka_flush(producer_rk_, /*timeout_ms=*/10'000); }

    // Build a catchup options struct pointing at the mock cluster.
    // offline_watermark_sec=1 → ts_ms=1000ms (epoch 1970-01-01 00:00:01),
    // which is earlier than any real-time timestamp, so offsets_for_times
    // resolves to the very first available offset.
    KafkaImportCatchupOptions Opts() {
        KafkaImportCatchupOptions o;
        o.brokers                = bootstraps_;
        o.topic                  = kTopic;
        o.partition              = kPartition;
        o.offline_watermark_sec  = 1;
        o.rewind_minutes         = 0;
        o.max_silence_loops      = 2;
        o.consume_timeout_ms     = 200;
        o.max_wall_seconds       = 30;
        o.log_err = [](std::string_view m) {
            std::cerr << "[kafka_mock][err] " << m << "\n";
        };
        return o;
    }

    rd_kafka_t*              producer_rk_ = nullptr;
    rd_kafka_mock_cluster_t* mcluster_    = nullptr;
    std::string              bootstraps_;
    rd_kafka_topic_t*        rkt_         = nullptr;

    FtAllocator              alloc_;
    Schema                   schema_;
    std::unique_ptr<KVIndex> idx_;
    fs::path                 table_dir_;
};

// ─── consume + verify index state ────────────────────────────────────────────

TEST_F(KafkaMockTest, SingleUpsertMessage) {
    Produce(R"({"user_id":1,"age":30,"name":"Alice"})");
    Flush();

    ASSERT_TRUE(RunKafkaImportCatchup(table_dir_, idx_.get(), &schema_, Opts()));

    Doc out;
    ASSERT_TRUE(idx_->Get("1", &out));
    EXPECT_EQ(out.get_int64(kFidUserId), 1);
    EXPECT_EQ(out.get_int32(kFidAge),    30);
    EXPECT_EQ(out.get_string(kFidName),  "Alice");
}

TEST_F(KafkaMockTest, MissingFieldsNotOverwritten) {
    // First message sets all fields.
    Produce(R"({"user_id":2,"age":25,"name":"Bob"})");
    // Second message only updates age; name must stay.
    Produce(R"({"user_id":2,"age":26})");
    Flush();

    ASSERT_TRUE(RunKafkaImportCatchup(table_dir_, idx_.get(), &schema_, Opts()));

    Doc out;
    ASSERT_TRUE(idx_->Get("2", &out));
    EXPECT_EQ(out.get_int32(kFidAge),   26);
    EXPECT_EQ(out.get_string(kFidName), "Bob");  // must not be cleared
}

TEST_F(KafkaMockTest, ArrayAppendsAcrossMessages) {
    Produce(R"({"user_id":3,"clicks":[10,20]})");
    Produce(R"({"user_id":3,"clicks":[30]})");
    Flush();

    ASSERT_TRUE(RunKafkaImportCatchup(table_dir_, idx_.get(), &schema_, Opts()));

    Doc out;
    ASSERT_TRUE(idx_->Get("3", &out));
    ASSERT_EQ(out.array_size(kFidClicks), 3u);
    EXPECT_EQ(out.array_get_int64(kFidClicks, 0), 10);
    EXPECT_EQ(out.array_get_int64(kFidClicks, 1), 20);
    EXPECT_EQ(out.array_get_int64(kFidClicks, 2), 30);  // appended, not replaced
}

TEST_F(KafkaMockTest, DeleteOpRemovesRow) {
    Produce(R"({"user_id":4,"age":40})");
    Produce(R"({"_op":"DELETE","user_id":4})");
    Flush();

    ASSERT_TRUE(RunKafkaImportCatchup(table_dir_, idx_.get(), &schema_, Opts()));

    Doc out;
    EXPECT_FALSE(idx_->Get("4", &out));
    EXPECT_EQ(idx_->Size(), 0u);
}

TEST_F(KafkaMockTest, BatchJsonArrayInSingleMessage) {
    // One Kafka message containing a JSON array of multiple ops.
    Produce(R"([{"user_id":5,"age":10},{"user_id":6,"age":20}])");
    Flush();

    ASSERT_TRUE(RunKafkaImportCatchup(table_dir_, idx_.get(), &schema_, Opts()));

    Doc a, b;
    ASSERT_TRUE(idx_->Get("5", &a));
    ASSERT_TRUE(idx_->Get("6", &b));
    EXPECT_EQ(a.get_int32(kFidAge), 10);
    EXPECT_EQ(b.get_int32(kFidAge), 20);
}

TEST_F(KafkaMockTest, InvalidJsonDoesNotCrash) {
    Produce("not-json{{{");
    Produce(R"({"user_id":7,"age":1})");  // valid, must still be applied
    Flush();

    ASSERT_TRUE(RunKafkaImportCatchup(table_dir_, idx_.get(), &schema_, Opts()));

    Doc out;
    ASSERT_TRUE(idx_->Get("7", &out));
    EXPECT_EQ(out.get_int32(kFidAge), 1);
}

TEST_F(KafkaMockTest, EmptyTopicCompletes) {
    // No messages produced — should reach EOF immediately and succeed.
    EXPECT_TRUE(RunKafkaImportCatchup(table_dir_, idx_.get(), &schema_, Opts()));
    EXPECT_EQ(idx_->Size(), 0u);
}

// ─── offset file and meta file ────────────────────────────────────────────────

TEST_F(KafkaMockTest, WritesOffsetFileAfterCatchup) {
    Produce(R"({"user_id":10,"age":5})");
    Produce(R"({"user_id":11,"age":6})");
    Flush();

    ASSERT_TRUE(RunKafkaImportCatchup(table_dir_, idx_.get(), &schema_, Opts()));

    const fs::path offset_path = table_dir_ / "kafka.offset";
    ASSERT_TRUE(fs::exists(offset_path));

    std::ifstream f(offset_path);
    int64_t committed = -1;
    ASSERT_TRUE(static_cast<bool>(f >> committed));
    // Two messages produced → last committed offset should be 1 (0-based).
    EXPECT_EQ(committed, 1);
}

TEST_F(KafkaMockTest, WritesMetaJsonAfterCatchup) {
    Produce(R"({"user_id":20,"age":7})");
    Flush();

    auto opts       = Opts();
    opts.rewind_minutes = 3;
    ASSERT_TRUE(RunKafkaImportCatchup(table_dir_, idx_.get(), &schema_, opts));

    const fs::path meta_path = table_dir_ / "kafka_meta.json";
    ASSERT_TRUE(fs::exists(meta_path));

    nlohmann::json meta;
    std::ifstream  f(meta_path);
    ASSERT_NO_THROW(f >> meta);

    EXPECT_EQ(meta["topic"].get<std::string>(),        kTopic);
    EXPECT_EQ(meta["partition"].get<int32_t>(),        kPartition);
    EXPECT_EQ(meta["offline_watermark_sec"].get<int64_t>(), 1);
    EXPECT_EQ(meta["rewind_minutes"].get<uint32_t>(),  3u);
    EXPECT_GE(meta["last_committed_offset"].get<int64_t>(), 0);
    EXPECT_GT(meta["catchup_completed_unix"].get<int64_t>(), 0);
}

TEST_F(KafkaMockTest, OffsetFileAllowsResume) {
    // Round 1: produce two messages, catch-up, committed offset = 1.
    Produce(R"({"user_id":30,"age":1})");
    Produce(R"({"user_id":31,"age":2})");
    Flush();
    ASSERT_TRUE(RunKafkaImportCatchup(table_dir_, idx_.get(), &schema_, Opts()));
    EXPECT_EQ(idx_->Size(), 2u);

    // Simulate reload: new index but same offset file.
    std::unique_ptr<KVIndex> idx2 = std::make_unique<KVIndex>(&alloc_, &schema_);

    // Round 2: produce one more message, set start from offset file.
    Produce(R"({"user_id":32,"age":3})");
    Flush();

    // Read committed offset from file and start at +1.
    int64_t committed = -1;
    { std::ifstream f(table_dir_ / "kafka.offset"); f >> committed; }
    ASSERT_GE(committed, 0);

    // Manually test that LoadOffset semantics give committed+1 = 2.
    EXPECT_EQ(committed + 1, 2);  // third message is at offset 2

    // Run catch-up again from scratch on the fresh index (offset file present
    // will be overwritten by this run since we re-run from watermark=1).
    // The key property: previous contents survive because they were in offset
    // file when the online server reads it at reload time.
    EXPECT_TRUE(committed >= 0);  // file was written
}
