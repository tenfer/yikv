// Process-internal RPC tests: FlatBuffers requests → db::Handle* → TableRegistry + KVIndex.
// No listening socket; verifies the same path used by brpc/gRPC services.

#include "db/handlers.h"
#include "table_registry.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <flatbuffers/flatbuffers.h>

#include "db/db.h"
#include "schema/schema.h"
#include "yikv_server_generated.h"

namespace fs = std::filesystem;
using yikv::db::DB;
using yikv::db::DBOptions;
using yikv::schema::Schema;
using yikv_server::TableRegistry;
using yikv_server::db::HandleBatchGet;
using yikv_server::db::HandleGet;
using yikv_server::db::HandlePut;
using yikv_server::db::HandlePutBatch;

namespace {

static const char* kSchemaJson = R"({
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

static const char* kTable = "t";

static const yikv::FieldValue* FindFv(const yikv::Row* row, uint16_t fid) {
    if (!row || !row->fields()) return nullptr;
    for (auto fv : *row->fields()) {
        if (fv && fv->field_id() == fid) return fv;
    }
    return nullptr;
}

static int64_t ReadI64(const yikv::Row* row, uint16_t fid) {
    const auto* fv = FindFv(row, fid);
    EXPECT_NE(fv, nullptr);
    if (!fv) return -1;
    EXPECT_EQ(fv->vtype(), yikv::ValueType_I64);
    return fv->i64();
}

static int32_t ReadI32(const yikv::Row* row, uint16_t fid) {
    const auto* fv = FindFv(row, fid);
    EXPECT_NE(fv, nullptr);
    if (!fv) return -1;
    EXPECT_EQ(fv->vtype(), yikv::ValueType_I32);
    return fv->i32();
}

static std::string ReadString(const yikv::Row* row, uint16_t fid) {
    const auto* fv = FindFv(row, fid);
    EXPECT_NE(fv, nullptr);
    if (!fv) return {};
    EXPECT_EQ(fv->vtype(), yikv::ValueType_STRING);
    return fv->s() ? fv->s()->str() : std::string{};
}

static std::string ReadFieldName(const yikv::Row* row, uint16_t fid) {
    const auto* fv = FindFv(row, fid);
    EXPECT_NE(fv, nullptr);
    if (!fv || !fv->field_name()) return {};
    return fv->field_name()->str();
}

static flatbuffers::Offset<yikv::Row> MakeRowBasic(flatbuffers::FlatBufferBuilder& fbb,
                                                   int64_t                        uid,
                                                   int32_t                        age,
                                                   const char*                    name) {
    std::vector<flatbuffers::Offset<yikv::FieldValue>> fvs;
    fvs.push_back(yikv::CreateFieldValue(fbb, 1, yikv::ValueType_I64, 0, uid, 0, 0, 0, 0, 0, 0, 0, 0,
                                         0));
    fvs.push_back(yikv::CreateFieldValue(fbb, 2, yikv::ValueType_I32, age, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                         0));
    auto name_off = fbb.CreateString(name);
    fvs.push_back(yikv::CreateFieldValue(fbb, 3, yikv::ValueType_STRING, 0, 0, 0, 0, name_off, 0, 0,
                                         0, 0, 0, 0));
    return yikv::CreateRow(fbb, fbb.CreateVector(fvs));
}

static flatbuffers::Offset<yikv::Row> MakeRowUidAgeOnly(flatbuffers::FlatBufferBuilder& fbb,
                                                        int64_t                        uid,
                                                        int32_t                        age) {
    std::vector<flatbuffers::Offset<yikv::FieldValue>> fvs;
    fvs.push_back(yikv::CreateFieldValue(fbb, 1, yikv::ValueType_I64, 0, uid, 0, 0, 0, 0, 0, 0, 0, 0,
                                         0));
    fvs.push_back(yikv::CreateFieldValue(fbb, 2, yikv::ValueType_I32, age, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                         0));
    return yikv::CreateRow(fbb, fbb.CreateVector(fvs));
}

static flatbuffers::Offset<yikv::Row> MakeRowWithClicks(flatbuffers::FlatBufferBuilder& fbb,
                                                         int64_t                         uid,
                                                         int32_t                         age,
                                                         const char*                     name,
                                                         const std::vector<int64_t>&    clicks) {
    std::vector<flatbuffers::Offset<yikv::FieldValue>> fvs;
    fvs.push_back(yikv::CreateFieldValue(fbb, 1, yikv::ValueType_I64, 0, uid, 0, 0, 0, 0, 0, 0, 0, 0,
                                         0));
    fvs.push_back(yikv::CreateFieldValue(fbb, 2, yikv::ValueType_I32, age, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                         0));
    auto name_off = fbb.CreateString(name);
    fvs.push_back(yikv::CreateFieldValue(fbb, 3, yikv::ValueType_STRING, 0, 0, 0, 0, name_off, 0, 0,
                                         0, 0, 0, 0));
    auto clk = clicks.empty() ? 0 : fbb.CreateVector<int64_t>(clicks);
    fvs.push_back(yikv::CreateFieldValue(fbb, 4, yikv::ValueType_ARR_I64, 0, 0, 0, 0, 0, 0, 0, clk, 0,
                                         0, 0));
    return yikv::CreateRow(fbb, fbb.CreateVector(fvs));
}

}  // namespace

class HandlersRpcTest : public ::testing::Test {
protected:
    void SetUp() override {
        char tmpl[] = "/tmp/yikv_hdl_testXXXXXX";
        ASSERT_NE(::mkdtemp(tmpl), nullptr);
        db_path_ = tmpl;

        DBOptions opt;
        opt.db_path              = db_path_;
        opt.exclusive_arena_lock = false;
        opt.alloc_defaults.arena_size   = 32ULL * 1024 * 1024;
        opt.alloc_defaults.segment_size = opt.alloc_defaults.arena_size;
        yikv::db::DB::Init(std::move(opt));

        Schema sch;
        std::string err;
        ASSERT_TRUE(sch.LoadJson(kSchemaJson, &err)) << err;
        DB::Instance().CreateKVIndex(kTable, sch, 10);

        reg_ = std::make_unique<TableRegistry>(fs::path(db_path_), /*default_brokers=*/"");
        reg_->ScanAndLoad();
        ASSERT_TRUE(reg_->Acquire(kTable).has_value());
    }

    void TearDown() override {
        reg_.reset();
        DB::Instance().CloseAll();
        DB::ResetForTest();
        std::error_code ec;
        fs::remove_all(db_path_, ec);
    }

    std::string GetRaw(const char* pk) {
        flatbuffers::FlatBufferBuilder fbb(128);
        auto                           req =
            yikv::CreateGetRequestDirect(fbb, pk, kTable);
        fbb.Finish(req);
        std::string resp;
        HandleGet(reg_.get(), fbb.GetBufferPointer(), fbb.GetSize(), &resp);
        return resp;
    }

    void PutRow(flatbuffers::FlatBufferBuilder& fbb,
                flatbuffers::Offset<yikv::Row>   row,
                bool                             expect_ok = true) {
        auto req = yikv::CreatePutRequest(fbb, row, fbb.CreateString(kTable));
        fbb.Finish(req);
        std::string resp;
        HandlePut(reg_.get(), fbb.GetBufferPointer(), fbb.GetSize(), &resp);
        const auto* pr =
            flatbuffers::GetRoot<yikv::PutResponse>(reinterpret_cast<const uint8_t*>(resp.data()));
        ASSERT_NE(pr, nullptr);
        if (expect_ok) {
            EXPECT_TRUE(pr->ok()) << (pr->err() ? pr->err()->c_str() : "");
        } else {
            EXPECT_FALSE(pr->ok());
            EXPECT_TRUE(pr->err() && pr->err()->size() > 0);
        }
    }

    std::string              db_path_;
    std::unique_ptr<TableRegistry> reg_;
};

TEST_F(HandlersRpcTest, PutThenGetRoundTrip) {
    flatbuffers::FlatBufferBuilder fbb(256);
    PutRow(fbb, MakeRowBasic(fbb, 42, 30, "Ada"));

    std::string                    gbuf = GetRaw("42");
    const auto*                    gr =
        flatbuffers::GetRoot<yikv::GetResponse>(reinterpret_cast<const uint8_t*>(gbuf.data()));
    ASSERT_TRUE(gr->found());
    ASSERT_NE(gr->row(), nullptr);
    EXPECT_EQ(ReadI64(gr->row(), 1), 42);
    EXPECT_EQ(ReadI32(gr->row(), 2), 30);
    EXPECT_EQ(ReadString(gr->row(), 3), "Ada");
    EXPECT_EQ(ReadFieldName(gr->row(), 1), "user_id");
    EXPECT_EQ(ReadFieldName(gr->row(), 2), "age");
    EXPECT_EQ(ReadFieldName(gr->row(), 3), "name");
}

TEST_F(HandlersRpcTest, GetMissingReturnsNotFound) {
    std::string gbuf = GetRaw("999");
    const auto* gr =
        flatbuffers::GetRoot<yikv::GetResponse>(reinterpret_cast<const uint8_t*>(gbuf.data()));
    EXPECT_FALSE(gr->found());
    EXPECT_EQ(gr->row(), nullptr);
}

TEST_F(HandlersRpcTest, UnknownTableReturnsErrorString) {
    flatbuffers::FlatBufferBuilder fbb(64);
    auto req = yikv::CreateGetRequestDirect(fbb, "1", "no_such_table");
    fbb.Finish(req);
    std::string resp;
    HandleGet(reg_.get(), fbb.GetBufferPointer(), fbb.GetSize(), &resp);
    const auto* gr =
        flatbuffers::GetRoot<yikv::GetResponse>(reinterpret_cast<const uint8_t*>(resp.data()));
    EXPECT_FALSE(gr->found());
    ASSERT_TRUE(gr->err());
    std::string e = gr->err()->str();
    EXPECT_NE(e.find("unknown table"), std::string::npos);
}

TEST_F(HandlersRpcTest, UpsertOverwritesWholeDocument) {
    // Put supplies only fields present in the Row; Upsert replaces the stored doc.
    // A second Put with only user_id+age clears name (RPC ≠ Kafka JSON merge).
    {
        flatbuffers::FlatBufferBuilder fbb(256);
        PutRow(fbb, MakeRowBasic(fbb, 7, 20, "First"));
    }
    {
        flatbuffers::FlatBufferBuilder fbb(256);
        PutRow(fbb, MakeRowUidAgeOnly(fbb, 7, 21));
    }
    std::string gbuf = GetRaw("7");
    const auto* gr =
        flatbuffers::GetRoot<yikv::GetResponse>(reinterpret_cast<const uint8_t*>(gbuf.data()));
    ASSERT_TRUE(gr->found());
    EXPECT_EQ(ReadI32(gr->row(), 2), 21);
    EXPECT_TRUE(ReadString(gr->row(), 3).empty());
}

TEST_F(HandlersRpcTest, PutBatchInsertsTwoRows) {
    flatbuffers::FlatBufferBuilder fbb(512);
    std::vector<flatbuffers::Offset<yikv::Row>> rows = {
        MakeRowBasic(fbb, 10, 1, "a"),
        MakeRowBasic(fbb, 11, 2, "b"),
    };
    auto req =
        yikv::CreatePutBatchRequest(fbb, fbb.CreateVector(rows), fbb.CreateString(kTable));
    fbb.Finish(req);
    std::string resp;
    HandlePutBatch(reg_.get(), fbb.GetBufferPointer(), fbb.GetSize(), &resp);
    const auto* pr =
        flatbuffers::GetRoot<yikv::PutBatchResponse>(reinterpret_cast<const uint8_t*>(resp.data()));
    ASSERT_TRUE(pr->ok());

    std::string buf10 = GetRaw("10");
    const auto* g10 =
        flatbuffers::GetRoot<yikv::GetResponse>(reinterpret_cast<const uint8_t*>(buf10.data()));
    ASSERT_TRUE(g10->found());
    EXPECT_EQ(ReadString(g10->row(), 3), "a");

    std::string buf11 = GetRaw("11");
    const auto* g11 =
        flatbuffers::GetRoot<yikv::GetResponse>(reinterpret_cast<const uint8_t*>(buf11.data()));
    ASSERT_TRUE(g11->found());
    EXPECT_EQ(ReadString(g11->row(), 3), "b");
}

TEST_F(HandlersRpcTest, BatchGetHitAndMiss) {
    {
        flatbuffers::FlatBufferBuilder fbb(128);
        PutRow(fbb, MakeRowBasic(fbb, 100, 3, "hit"));
    }
    flatbuffers::FlatBufferBuilder fbb(256);
    std::vector<flatbuffers::Offset<flatbuffers::String>> pks = {
        fbb.CreateString("100"), fbb.CreateString("missing")};
    auto req = yikv::CreateBatchGetRequest(fbb, fbb.CreateVector(pks), fbb.CreateString(kTable));
    fbb.Finish(req);
    std::string resp;
    HandleBatchGet(reg_.get(), fbb.GetBufferPointer(), fbb.GetSize(), &resp);
    const auto* br =
        flatbuffers::GetRoot<yikv::BatchGetResponse>(reinterpret_cast<const uint8_t*>(resp.data()));
    ASSERT_TRUE(br->rows());
    ASSERT_EQ(br->rows()->size(), 2u);
    const auto* r0 = br->rows()->Get(0);
    const auto* r1 = br->rows()->Get(1);
    ASSERT_NE(r0, nullptr);
    ASSERT_NE(r1, nullptr);
    EXPECT_EQ(ReadString(r0, 3), "hit");
    EXPECT_EQ(FindFv(r1, 1), nullptr);
}

TEST_F(HandlersRpcTest, PutWrongValueTypeFails) {
    flatbuffers::FlatBufferBuilder fbb(128);
    std::vector<flatbuffers::Offset<yikv::FieldValue>> fvs;
    fvs.push_back(yikv::CreateFieldValue(fbb, 1, yikv::ValueType_I64, 0, 1LL, 0, 0, 0, 0, 0, 0, 0, 0,
                                         0));
    // name (field 3) must be STRING, send I32
    fvs.push_back(yikv::CreateFieldValue(fbb, 3, yikv::ValueType_I32, 99, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                         0));
    auto row = yikv::CreateRow(fbb, fbb.CreateVector(fvs));
    PutRow(fbb, row, /*expect_ok=*/false);
}

TEST_F(HandlersRpcTest, PutUnknownFieldIdFails) {
    flatbuffers::FlatBufferBuilder fbb(128);
    std::vector<flatbuffers::Offset<yikv::FieldValue>> fvs;
    fvs.push_back(yikv::CreateFieldValue(fbb, 1, yikv::ValueType_I64, 0, 2LL, 0, 0, 0, 0, 0, 0, 0, 0,
                                         0));
    fvs.push_back(yikv::CreateFieldValue(fbb, 99, yikv::ValueType_I32, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                         0));
    auto row = yikv::CreateRow(fbb, fbb.CreateVector(fvs));
    PutRow(fbb, row, /*expect_ok=*/false);
}

TEST_F(HandlersRpcTest, Int64ArrayRoundTrip) {
    flatbuffers::FlatBufferBuilder fbb(256);
    PutRow(fbb, MakeRowWithClicks(fbb, 55, 9, "z", {100, 200}));

    std::string     gbuf = GetRaw("55");
    const auto*     gr =
        flatbuffers::GetRoot<yikv::GetResponse>(reinterpret_cast<const uint8_t*>(gbuf.data()));
    ASSERT_TRUE(gr->found());
    const auto* fv = FindFv(gr->row(), 4);
    ASSERT_NE(fv, nullptr);
    EXPECT_EQ(fv->vtype(), yikv::ValueType_ARR_I64);
    ASSERT_TRUE(fv->ai64());
    ASSERT_EQ(fv->ai64()->size(), 2u);
    EXPECT_EQ(fv->ai64()->Get(0), 100);
    EXPECT_EQ(fv->ai64()->Get(1), 200);
}
