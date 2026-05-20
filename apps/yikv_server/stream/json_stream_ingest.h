#pragma once

// Shared JSON → KVIndex ingest for Kafka and offline catch-up.
// Merge rules: keys absent from the JSON object leave existing column values
// unchanged; array fields append elements from the payload.

#include <functional>
#include <string_view>

#include <nlohmann/json.hpp>

namespace yikv::index {
class Doc;
class KVIndex;
}  // namespace yikv::index
namespace yikv::schema {
class Schema;
}  // namespace yikv::schema

namespace yikv_server::stream {

using LogFn = std::function<void(std::string_view)>;

// Copy all schema fields from src into dst (dst must be a fresh NewDoc()).
void CopyDocFromSchema(const yikv::schema::Schema*              schema,
                       const yikv::index::Doc&                  src,
                       yikv::index::Doc*                        dst);

// Apply JSON fields onto doc: only keys present in obj are written; arrays append.
// Skips _op and _ts. Unknown field names are ignored. json null for a key is skipped.
bool MergeJsonIntoDoc(const yikv::schema::Schema* schema,
                      yikv::index::Doc*           doc,
                      const nlohmann::json&       obj,
                      const LogFn&                log_err);

bool ExtractPkString(const yikv::schema::Schema* schema,
                     const nlohmann::json&       obj,
                     std::string*                pk_out,
                     const LogFn&                log_err);

bool ApplyStreamJsonObject(yikv::index::KVIndex*         idx,
                           const yikv::schema::Schema*   schema,
                           const nlohmann::json&         obj,
                           const LogFn&                  log_err);

}  // namespace yikv_server::stream
