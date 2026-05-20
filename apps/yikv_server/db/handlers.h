#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include <flatbuffers/flatbuffers.h>

#include "yikv_server_generated.h"

namespace yikv::index {
class Doc;
class KVIndex;
}  // namespace yikv::index
namespace yikv::schema {
class Schema;
struct FieldDef;
}  // namespace yikv::schema
namespace yikv_server {
class TableRegistry;
}

namespace yikv_server::db {

struct HandlerStatus {
    bool        ok{true};
    std::string err;
    static HandlerStatus Ok() { return {}; }
    static HandlerStatus Fail(std::string e) {
        HandlerStatus s;
        s.ok  = false;
        s.err = std::move(e);
        return s;
    }
};

// ─── Low-level helpers (used by handlers and KafkaSource) ────────────────────

HandlerStatus ApplyRowToDoc(yikv::index::Doc* doc, const yikv::Row* row,
                            const yikv::schema::Schema* schema);

std::string ExtractPkString(const yikv::index::Doc& doc,
                            const yikv::schema::Schema* schema);

flatbuffers::Offset<yikv::Row> BuildRow(flatbuffers::FlatBufferBuilder& fbb,
                                            const yikv::index::Doc&        doc,
                                            const yikv::schema::Schema*    schema);

// ─── RPC entry-points ────────────────────────────────────────────────────────
// Each handler reads table_name from the FlatBuffers request, looks up the
// table in the registry, and dispatches to the appropriate KVIndex operation.

void HandleGet     (TableRegistry* reg, const void* req, size_t req_len, std::string* out_resp);
void HandlePut     (TableRegistry* reg, const void* req, size_t req_len, std::string* out_resp);
void HandlePutBatch(TableRegistry* reg, const void* req, size_t req_len, std::string* out_resp);
void HandleBatchGet(TableRegistry* reg, const void* req, size_t req_len, std::string* out_resp);

}  // namespace yikv_server::db
