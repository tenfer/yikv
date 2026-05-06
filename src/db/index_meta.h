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
};

const char* IndexKindName(IndexKind k);

bool LoadIndexMeta(const std::string& json, IndexMeta* out, std::string* err);
std::string SerializeIndexMeta(const IndexMeta& m);

}  // namespace db
}  // namespace yikv
