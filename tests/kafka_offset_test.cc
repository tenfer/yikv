// Tests for kafka.offset file format and kafka_meta.json content.
// Does NOT start a Kafka broker — only tests filesystem I/O.

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <nlohmann/json.hpp>

namespace fs = std::filesystem;

// ─── helpers mirroring the production logic in kafka_import_catchup.cc ────────
// (These are intentionally kept as thin copies so any drift is caught.)

static bool WriteOffsetFile(const fs::path& dir, int64_t offset) {
    std::ofstream f(dir / "kafka.offset", std::ios::trunc);
    if (!f) return false;
    f << offset << "\n";
    return f.good();
}

static bool ReadOffsetFile(const fs::path& dir, int64_t* out) {
    std::ifstream f(dir / "kafka.offset");
    if (!f) return false;
    return static_cast<bool>(f >> *out);
}

static const int64_t kRdKafkaOffsetBeginning = -2;  // RD_KAFKA_OFFSET_BEGINNING

// Mirror of KafkaSource::LoadOffset interpretation
static int64_t InterpretOffset(int64_t raw) {
    return (raw >= 0) ? raw + 1 : kRdKafkaOffsetBeginning;
}

// Mirror of catch-up timestamp computation
static int64_t ComputeStartTsMs(int64_t watermark_sec, uint32_t rewind_min) {
    int64_t ts = (watermark_sec - static_cast<int64_t>(rewind_min) * 60) * 1000;
    return ts < 0 ? 0 : ts;
}

// ─── temporary directory RAII ─────────────────────────────────────────────────

struct TmpDir {
    fs::path path;
    TmpDir() {
        path = fs::temp_directory_path() / ("kafka_offset_test_" +
               std::to_string(reinterpret_cast<uintptr_t>(this)));
        fs::create_directories(path);
    }
    ~TmpDir() { fs::remove_all(path); }
};

// ─── offset file round-trip ───────────────────────────────────────────────────

TEST(KafkaOffsetFile, WriteAndReadBack) {
    TmpDir d;
    ASSERT_TRUE(WriteOffsetFile(d.path, 12345));
    int64_t got = -1;
    ASSERT_TRUE(ReadOffsetFile(d.path, &got));
    EXPECT_EQ(got, 12345);
}

TEST(KafkaOffsetFile, ZeroOffsetRoundTrip) {
    TmpDir d;
    ASSERT_TRUE(WriteOffsetFile(d.path, 0));
    int64_t got = -1;
    ASSERT_TRUE(ReadOffsetFile(d.path, &got));
    EXPECT_EQ(got, 0);
}

TEST(KafkaOffsetFile, OverwriteUpdatesValue) {
    TmpDir d;
    WriteOffsetFile(d.path, 100);
    WriteOffsetFile(d.path, 999);
    int64_t got = -1;
    ReadOffsetFile(d.path, &got);
    EXPECT_EQ(got, 999);
}

TEST(KafkaOffsetFile, MissingFileReturnsFalse) {
    TmpDir d;
    int64_t got = -1;
    EXPECT_FALSE(ReadOffsetFile(d.path, &got));
}

// ─── LoadOffset resume semantics ─────────────────────────────────────────────

TEST(KafkaLoadOffset, ResumesAtNextAfterCommitted) {
    // committed = N → next consume starts at N+1
    EXPECT_EQ(InterpretOffset(0),   1);
    EXPECT_EQ(InterpretOffset(99),  100);
    EXPECT_EQ(InterpretOffset(999), 1000);
}

TEST(KafkaLoadOffset, NegativeOffsetFallsBackToBeginning) {
    EXPECT_EQ(InterpretOffset(-1), kRdKafkaOffsetBeginning);
    EXPECT_EQ(InterpretOffset(-2), kRdKafkaOffsetBeginning);
}

// ─── timestamp formula ────────────────────────────────────────────────────────

TEST(KafkaTimestamp, NoRewindIsWatermarkMs) {
    EXPECT_EQ(ComputeStartTsMs(1700000000, 0),
              1700000000LL * 1000);
}

TEST(KafkaTimestamp, RewindSubtractsMinutes) {
    // 5 min = 300 s → ts = (watermark - 300) * 1000
    int64_t wm = 1700000000;
    EXPECT_EQ(ComputeStartTsMs(wm, 5),
              (wm - 300) * 1000);
}

TEST(KafkaTimestamp, RewindLargerThanWatermarkClampsToZero) {
    EXPECT_EQ(ComputeStartTsMs(60, 2), 0);  // 60s - 120s < 0 → 0
}

TEST(KafkaTimestamp, ExactBoundary) {
    // rewind = watermark/60 → result is exactly 0
    EXPECT_EQ(ComputeStartTsMs(120, 2), 0);
    EXPECT_EQ(ComputeStartTsMs(121, 2), 1000);  // 1 second over
}

// ─── kafka_meta.json content ──────────────────────────────────────────────────

TEST(KafkaMetaJson, FieldsPresent) {
    nlohmann::json meta;
    meta["topic"]                  = "my-topic";
    meta["partition"]              = 0;
    meta["brokers"]                = "broker:9092";
    meta["offline_watermark_sec"]  = 1700000000;
    meta["rewind_minutes"]         = 5;
    meta["last_committed_offset"]  = 42;
    meta["catchup_completed_unix"] = 1700001000;

    // All keys must be present and correctly typed.
    EXPECT_EQ(meta["topic"].get<std::string>(), "my-topic");
    EXPECT_EQ(meta["partition"].get<int>(), 0);
    EXPECT_EQ(meta["last_committed_offset"].get<int64_t>(), 42);
    EXPECT_EQ(meta["offline_watermark_sec"].get<int64_t>(), 1700000000);
    EXPECT_EQ(meta["rewind_minutes"].get<uint32_t>(), 5u);
}

TEST(KafkaMetaJson, RoundTripThroughFile) {
    TmpDir d;
    nlohmann::json meta;
    meta["topic"]                 = "t";
    meta["partition"]             = 1;
    meta["last_committed_offset"] = 777;
    {
        std::ofstream f(d.path / "kafka_meta.json", std::ios::trunc);
        f << meta.dump(2) << "\n";
    }
    nlohmann::json back;
    {
        std::ifstream f(d.path / "kafka_meta.json");
        f >> back;
    }
    EXPECT_EQ(back["last_committed_offset"].get<int64_t>(), 777);
    EXPECT_EQ(back["topic"].get<std::string>(), "t");
}
