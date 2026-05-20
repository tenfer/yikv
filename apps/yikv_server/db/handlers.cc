#include "db/handlers.h"
#include "metrics/metrics.h"
#include "table_registry.h"

#include <chrono>

#include "index/doc.h"
#include "index/kv_index.h"
#include "schema/schema.h"

#include <cstring>
#include <optional>
#include <string_view>
#include <vector>

namespace yikv_server::db {

using yikv::index::Doc;
using yikv::index::KVIndex;
using yikv::schema::DataType;
using yikv::schema::FieldDef;
using yikv::schema::Schema;
using ::yikv_server::TableHandle;
using ::yikv_server::TableSlot;

// ─── Internal helpers ─────────────────────────────────────────────────────────

static std::string FieldTypeMismatch(const FieldDef& def, yikv::ValueType vt) {
    return "field_id " + std::to_string(def.field_id) + " type mismatch for schema "
           + std::string(yikv::schema::DataTypeName(def.type));
}

static bool FieldValueMatchesSchema(const FieldDef& def, yikv::ValueType vt) {
    if (def.is_array) {
        switch (def.type) {
            case DataType::Int32:   return vt == yikv::ValueType_ARR_I32;
            case DataType::Int64:   return vt == yikv::ValueType_ARR_I64;
            case DataType::Float32: return vt == yikv::ValueType_ARR_F32;
            case DataType::Float64: return vt == yikv::ValueType_ARR_F64;
            case DataType::String:  return vt == yikv::ValueType_ARR_STRING;
            default:                return false;
        }
    }
    switch (def.type) {
        case DataType::Bool:    return vt == yikv::ValueType_BOOL;
        case DataType::Int32:   return vt == yikv::ValueType_I32;
        case DataType::Int64:   return vt == yikv::ValueType_I64;
        case DataType::Float32: return vt == yikv::ValueType_F32;
        case DataType::Float64: return vt == yikv::ValueType_F64;
        case DataType::String:  return vt == yikv::ValueType_STRING;
        case DataType::Bytes:   return vt == yikv::ValueType_BYTES;
    }
    return false;
}

HandlerStatus ApplyRowToDoc(Doc* doc, const yikv::Row* row, const Schema* schema) {
    if (!row || !row->fields()) return HandlerStatus::Ok();
    for (auto fv : *row->fields()) {
        if (!fv) continue;
        const FieldDef* def = schema->FindFieldById(static_cast<uint16_t>(fv->field_id()));
        if (!def) {
            return HandlerStatus::Fail("unknown field_id " + std::to_string(fv->field_id()));
        }
        if (!FieldValueMatchesSchema(*def, fv->vtype())) {
            return HandlerStatus::Fail(FieldTypeMismatch(*def, fv->vtype()));
        }
        const uint32_t fid = def->field_id;
        switch (fv->vtype()) {
            case yikv::ValueType_BOOL:   doc->put_int32(fid, fv->i32() ? 1 : 0); break;
            case yikv::ValueType_I32:    doc->put_int32(fid, fv->i32());          break;
            case yikv::ValueType_I64:    doc->put_int64(fid, fv->i64());          break;
            case yikv::ValueType_F32:    doc->put_float(fid, fv->f32());          break;
            case yikv::ValueType_F64:    doc->put_double(fid, fv->f64());         break;
            case yikv::ValueType_STRING:
                doc->put_string(fid, fv->s() ? fv->s()->str() : ""); break;
            case yikv::ValueType_BYTES:
                if (fv->raw())
                    doc->put_string(fid, std::string_view(
                        reinterpret_cast<const char*>(fv->raw()->Data()), fv->raw()->size()));
                else
                    doc->put_string(fid, "");
                break;
            case yikv::ValueType_ARR_I32:
                if (fv->ai32())
                    doc->array_put_int32(fid, fv->ai32()->data(),
                                         static_cast<uint32_t>(fv->ai32()->size()));
                break;
            case yikv::ValueType_ARR_I64:
                if (fv->ai64())
                    doc->array_put_int64(fid, fv->ai64()->data(),
                                         static_cast<uint32_t>(fv->ai64()->size()));
                break;
            case yikv::ValueType_ARR_F32:
                if (fv->af32())
                    doc->array_put_float(fid, fv->af32()->data(),
                                         static_cast<uint32_t>(fv->af32()->size()));
                break;
            case yikv::ValueType_ARR_F64:
                if (fv->af64())
                    doc->array_put_double(fid, fv->af64()->data(),
                                          static_cast<uint32_t>(fv->af64()->size()));
                break;
            case yikv::ValueType_ARR_STRING: {
                if (!fv->as()) break;
                const auto* vec = fv->as();
                std::vector<std::string_view> parts;
                parts.reserve(vec->size());
                for (flatbuffers::uoffset_t i = 0; i < vec->size(); ++i) {
                    const auto* st = vec->Get(i);
                    parts.push_back(st ? std::string_view(st->c_str(), st->size())
                                       : std::string_view{});
                }
                doc->array_put_string(fid, parts.data(), static_cast<uint32_t>(parts.size()));
                break;
            }
            default:
                return HandlerStatus::Fail("unsupported ValueType for field_id "
                                           + std::to_string(fv->field_id()));
        }
    }
    return HandlerStatus::Ok();
}

std::string ExtractPkString(const Doc& doc, const Schema* schema) {
    const FieldDef* pk = schema->FindField(schema->pk());
    if (!pk) return {};
    const uint32_t fid = pk->field_id;
    switch (pk->type) {
        case DataType::Bool:   return std::to_string(doc.get_int32(fid));
        case DataType::Int32:  return std::to_string(doc.get_int32(fid));
        case DataType::Int64:  return std::to_string(doc.get_int64(fid));
        case DataType::String: return std::string(doc.get_string(fid));
        default:               return {};
    }
}

// ─── BuildRow ────────────────────────────────────────────────────────────────

static flatbuffers::Offset<yikv::FieldValue> BuildFieldValue(
    flatbuffers::FlatBufferBuilder& fbb, const Doc& doc, const FieldDef& def) {
    const uint32_t                fid           = def.field_id;
    const auto                    field_name_off = fbb.CreateString(def.name);
    if (def.is_array) {
        switch (def.type) {
            case DataType::Int32: {
                auto v   = doc.array_view_int32(fid);
                auto vec = v.second ? fbb.CreateVector(v.first, v.second) : 0;
                return yikv::CreateFieldValue(fbb, static_cast<uint16_t>(fid),
                    yikv::ValueType_ARR_I32, 0,0,0,0,0,0, vec,0,0,0,0, field_name_off);
            }
            case DataType::Int64: {
                auto v   = doc.array_view_int64(fid);
                auto vec = v.second ? fbb.CreateVector(v.first, v.second) : 0;
                return yikv::CreateFieldValue(fbb, static_cast<uint16_t>(fid),
                    yikv::ValueType_ARR_I64, 0,0,0,0,0,0, 0,vec,0,0,0, field_name_off);
            }
            case DataType::Float32: {
                auto v   = doc.array_view_float(fid);
                auto vec = v.second ? fbb.CreateVector(v.first, v.second) : 0;
                return yikv::CreateFieldValue(fbb, static_cast<uint16_t>(fid),
                    yikv::ValueType_ARR_F32, 0,0,0,0,0,0, 0,0,vec,0,0, field_name_off);
            }
            case DataType::Float64: {
                auto v   = doc.array_view_double(fid);
                auto vec = v.second ? fbb.CreateVector(v.first, v.second) : 0;
                return yikv::CreateFieldValue(fbb, static_cast<uint16_t>(fid),
                    yikv::ValueType_ARR_F64, 0,0,0,0,0,0, 0,0,0,vec,0, field_name_off);
            }
            case DataType::String: {
                const uint32_t n = doc.array_size(fid);
                std::vector<flatbuffers::Offset<flatbuffers::String>> strs;
                strs.reserve(n);
                for (uint32_t i = 0; i < n; ++i) {
                    auto sv = doc.array_get_string(fid, i);
                    strs.push_back(fbb.CreateString(sv.data(), sv.size()));
                }
                auto vec = n ? fbb.CreateVector(strs) : 0;
                return yikv::CreateFieldValue(fbb, static_cast<uint16_t>(fid),
                    yikv::ValueType_ARR_STRING, 0,0,0,0,0,0, 0,0,0,0, vec, field_name_off);
            }
            default:
                return yikv::CreateFieldValue(fbb, static_cast<uint16_t>(fid),
                    yikv::ValueType_NONE, 0,0,0,0,0,0, 0,0,0,0,0, field_name_off);
        }
    }
    switch (def.type) {
        case DataType::Bool:
            return yikv::CreateFieldValue(fbb, static_cast<uint16_t>(fid),
                yikv::ValueType_BOOL, doc.get_int32(fid) ? 1 : 0, 0,0,0,0,0, 0,0,0,0,0, field_name_off);
        case DataType::Int32:
            return yikv::CreateFieldValue(fbb, static_cast<uint16_t>(fid),
                yikv::ValueType_I32, doc.get_int32(fid), 0,0,0,0,0, 0,0,0,0,0, field_name_off);
        case DataType::Int64:
            return yikv::CreateFieldValue(fbb, static_cast<uint16_t>(fid),
                yikv::ValueType_I64, 0, doc.get_int64(fid), 0,0,0,0, 0,0,0,0,0, field_name_off);
        case DataType::Float32:
            return yikv::CreateFieldValue(fbb, static_cast<uint16_t>(fid),
                yikv::ValueType_F32, 0,0, doc.get_float(fid), 0,0,0, 0,0,0,0,0, field_name_off);
        case DataType::Float64:
            return yikv::CreateFieldValue(fbb, static_cast<uint16_t>(fid),
                yikv::ValueType_F64, 0,0,0, doc.get_double(fid), 0,0, 0,0,0,0,0, field_name_off);
        case DataType::String: {
            auto sv = doc.get_string(fid);
            auto s  = fbb.CreateString(sv.data(), sv.size());
            return yikv::CreateFieldValue(fbb, static_cast<uint16_t>(fid),
                yikv::ValueType_STRING, 0,0,0,0, s, 0, 0,0,0,0,0, field_name_off);
        }
        case DataType::Bytes: {
            auto sv = doc.get_string(fid);
            auto b  = fbb.CreateVector(reinterpret_cast<const uint8_t*>(sv.data()), sv.size());
            return yikv::CreateFieldValue(fbb, static_cast<uint16_t>(fid),
                yikv::ValueType_BYTES, 0,0,0,0, 0, b, 0,0,0,0,0, field_name_off);
        }
    }
    return yikv::CreateFieldValue(fbb, static_cast<uint16_t>(fid),
        yikv::ValueType_NONE, 0,0,0,0,0,0, 0,0,0,0,0, field_name_off);
}

flatbuffers::Offset<yikv::Row> BuildRow(flatbuffers::FlatBufferBuilder& fbb,
                                            const Doc& doc, const Schema* schema) {
    std::vector<flatbuffers::Offset<yikv::FieldValue>> offs;
    for (const auto& fp : schema->fields())
        offs.push_back(BuildFieldValue(fbb, doc, *fp));
    return yikv::CreateRow(fbb, fbb.CreateVector(offs));
}

// ─── Table lookup helper ─────────────────────────────────────────────────────

template <typename ErrFn>
static std::optional<TableHandle> AcquireOrError(
    TableRegistry*                  reg,
    const flatbuffers::String*      tname_fb,
    flatbuffers::FlatBufferBuilder& fbb,
    std::string*                    out_resp,
    ErrFn                           make_error_resp) {
    if (!tname_fb || tname_fb->size() == 0) {
        make_error_resp(fbb, "missing table_name in request", out_resp);
        return std::nullopt;
    }
    auto h = reg->Acquire(tname_fb->str());
    if (!h) {
        make_error_resp(fbb, "unknown table: " + tname_fb->str(), out_resp);
        return std::nullopt;
    }
    return h;
}

// ─── HandleGet ───────────────────────────────────────────────────────────────

void HandleGet(TableRegistry* reg, const void* req, size_t req_len, std::string* out_resp) {
    auto _scope = metrics::Metrics::instance().rpc.scope("get");
    const yikv::GetRequest* preq =
        flatbuffers::GetRoot<yikv::GetRequest>(reinterpret_cast<const uint8_t*>(req));
    flatbuffers::FlatBufferBuilder fbb(1024);

    auto error_resp = [&_scope](flatbuffers::FlatBufferBuilder& fbb, const std::string& msg,
                                std::string* out) {
        _scope.mark_error();
        auto er = fbb.CreateString(msg);
        fbb.Finish(yikv::CreateGetResponse(fbb, false, er, 0, 0));
        out->assign(reinterpret_cast<const char*>(fbb.GetBufferPointer()), fbb.GetSize());
    };

    if (!preq || !preq->pk()) { error_resp(fbb, "missing pk", out_resp); return; }

    auto h = AcquireOrError(reg, preq->table_name(), fbb, out_resp, error_resp);
    if (!h) return;
    TableSlot& slot = **h;

    std::string pk = preq->pk()->str();
    Doc         out_doc;
    uint64_t    ns = 0;
    bool        hit;
    {
        auto t0 = std::chrono::steady_clock::now();
        hit     = slot.kv->Get(pk, &out_doc);
        ns      = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now() - t0).count());
    }
    auto er = fbb.CreateString("");
    if (!hit) {
        fbb.Finish(yikv::CreateGetResponse(fbb, false, er, 0, ns));
    } else {
        auto row = BuildRow(fbb, out_doc, slot.schema);
        fbb.Finish(yikv::CreateGetResponse(fbb, true, er, row, ns));
    }
    out_resp->assign(reinterpret_cast<const char*>(fbb.GetBufferPointer()), fbb.GetSize());
}

// ─── HandlePut ───────────────────────────────────────────────────────────────

void HandlePut(TableRegistry* reg, const void* req, size_t req_len, std::string* out_resp) {
    auto _scope = metrics::Metrics::instance().rpc.scope("put");
    const yikv::PutRequest* preq =
        flatbuffers::GetRoot<yikv::PutRequest>(reinterpret_cast<const uint8_t*>(req));
    flatbuffers::FlatBufferBuilder fbb(256);

    auto error_resp = [&_scope](flatbuffers::FlatBufferBuilder& fbb, const std::string& msg,
                                std::string* out) {
        _scope.mark_error();
        auto es = fbb.CreateString(msg);
        fbb.Finish(yikv::CreatePutResponse(fbb, false, es));
        out->assign(reinterpret_cast<const char*>(fbb.GetBufferPointer()), fbb.GetSize());
    };

    if (!preq || !preq->row()) { error_resp(fbb, "missing row", out_resp); return; }

    auto h = AcquireOrError(reg, preq->table_name(), fbb, out_resp, error_resp);
    if (!h) return;
    TableSlot& slot = **h;

    {
        std::lock_guard lk(slot.write_mu);
        Doc doc = slot.kv->NewDoc();
        HandlerStatus st = ApplyRowToDoc(&doc, preq->row(), slot.schema);
        if (!st.ok) { error_resp(fbb, st.err, out_resp); return; }
        std::string pk = ExtractPkString(doc, slot.schema);
        if (pk.empty()) {
            error_resp(fbb, "cannot derive pk (check pk field type in schema)", out_resp);
            return;
        }
        slot.kv->Upsert(&doc);
    }
    auto es = fbb.CreateString("");
    fbb.Finish(yikv::CreatePutResponse(fbb, true, es));
    out_resp->assign(reinterpret_cast<const char*>(fbb.GetBufferPointer()), fbb.GetSize());
}

// ─── HandlePutBatch ──────────────────────────────────────────────────────────

void HandlePutBatch(TableRegistry* reg, const void* req, size_t req_len, std::string* out_resp) {
    auto _scope = metrics::Metrics::instance().rpc.scope("put_batch");
    const yikv::PutBatchRequest* preq =
        flatbuffers::GetRoot<yikv::PutBatchRequest>(reinterpret_cast<const uint8_t*>(req));
    flatbuffers::FlatBufferBuilder fbb(256);

    auto error_resp = [&_scope](flatbuffers::FlatBufferBuilder& fbb, const std::string& msg,
                                std::string* out) {
        _scope.mark_error();
        auto es = fbb.CreateString(msg);
        fbb.Finish(yikv::CreatePutBatchResponse(fbb, false, es));
        out->assign(reinterpret_cast<const char*>(fbb.GetBufferPointer()), fbb.GetSize());
    };

    if (!preq || !preq->rows()) { error_resp(fbb, "missing rows", out_resp); return; }

    auto h = AcquireOrError(reg, preq->table_name(), fbb, out_resp, error_resp);
    if (!h) return;
    TableSlot& slot = **h;

    const auto* rows = preq->rows();
    if (rows->size() == 0) { error_resp(fbb, "empty batch", out_resp); return; }

    {
        std::lock_guard lk(slot.write_mu);
        std::vector<Doc> staged;
        staged.reserve(rows->size());
        for (flatbuffers::uoffset_t i = 0; i < rows->size(); ++i) {
            const yikv::Row* row = rows->Get(i);
            if (!row) { error_resp(fbb, "null row in batch", out_resp); return; }
            Doc doc = slot.kv->NewDoc();
            HandlerStatus st = ApplyRowToDoc(&doc, row, slot.schema);
            if (!st.ok) { error_resp(fbb, st.err, out_resp); return; }
            std::string pk = ExtractPkString(doc, slot.schema);
            if (pk.empty()) {
                error_resp(fbb, "cannot derive pk", out_resp);
                return;
            }
            staged.push_back(doc);
        }
        std::vector<Doc*> ptrs;
        ptrs.reserve(staged.size());
        for (auto& doc : staged) ptrs.push_back(&doc);
        slot.kv->BatchUpsert(ptrs);
    }
    auto es = fbb.CreateString("");
    fbb.Finish(yikv::CreatePutBatchResponse(fbb, true, es));
    out_resp->assign(reinterpret_cast<const char*>(fbb.GetBufferPointer()), fbb.GetSize());
}

// ─── HandleBatchGet ──────────────────────────────────────────────────────────

void HandleBatchGet(TableRegistry* reg, const void* req, size_t req_len, std::string* out_resp) {
    auto _scope = metrics::Metrics::instance().rpc.scope("batch_get");
    const yikv::BatchGetRequest* preq =
        flatbuffers::GetRoot<yikv::BatchGetRequest>(reinterpret_cast<const uint8_t*>(req));
    flatbuffers::FlatBufferBuilder fbb(4096);

    auto error_resp = [&_scope](flatbuffers::FlatBufferBuilder& fbb, const std::string& msg,
                                std::string* out) {
        _scope.mark_error();
        auto es = fbb.CreateString(msg);
        fbb.Finish(yikv::CreateBatchGetResponse(fbb, 0, es));
        out->assign(reinterpret_cast<const char*>(fbb.GetBufferPointer()), fbb.GetSize());
    };

    if (!preq || !preq->pks()) { error_resp(fbb, "missing pks", out_resp); return; }

    auto h = AcquireOrError(reg, preq->table_name(), fbb, out_resp, error_resp);
    if (!h) return;
    TableSlot& slot = **h;

    std::vector<flatbuffers::Offset<yikv::Row>> row_offs;
    const auto* pks = preq->pks();
    for (flatbuffers::uoffset_t i = 0; i < pks->size(); ++i) {
        const auto* pk_s = pks->Get(i);
        if (!pk_s) { row_offs.push_back(yikv::CreateRow(fbb, 0)); continue; }
        Doc out_doc;
        if (!slot.kv->Get(pk_s->str(), &out_doc))
            row_offs.push_back(yikv::CreateRow(fbb, 0));
        else
            row_offs.push_back(BuildRow(fbb, out_doc, slot.schema));
    }
    auto es = fbb.CreateString("");
    fbb.Finish(yikv::CreateBatchGetResponse(fbb, fbb.CreateVector(row_offs), es));
    out_resp->assign(reinterpret_cast<const char*>(fbb.GetBufferPointer()), fbb.GetSize());
}

}  // namespace yikv_server::db
