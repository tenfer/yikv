#pragma once

#include <cstdint>
#include <string>

namespace yikv {
namespace db {

enum class IndexKind : std::uint8_t {
    KV = 0,
    Inverted = 1,
};

struct IndexMeta {
    IndexKind     kind = IndexKind::KV;
    std::uint64_t index_hdr_off   = 0;
    std::uint64_t docs_hdr_off    = 0;
    std::uint64_t posting_hdr_off = 0;
    // Optional book-keeping (refreshed by PersistIndexMeta / end of import). Loaded as 0 if absent.
    std::uint64_t record_count = 0;   // KV doc count (HashMap entries)
    std::uint64_t arena_bytes  = 0;   // Sum of on-disk arena segment file sizes (arena + arena.segN)
};

const char* IndexKindName(IndexKind k);

bool LoadIndexMeta(const std::string& json, IndexMeta* out, std::string* err);
std::string SerializeIndexMeta(const IndexMeta& m);

}  // namespace db
}  // namespace yikv
