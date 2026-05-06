#include "src/db/index_meta.h"

#include <cctype>
#include <cstdlib>
#include <sstream>
#include <string_view>

namespace yikv {
namespace db {

const char* IndexKindName(IndexKind k) {
    switch (k) {
        case IndexKind::KV:
            return "kv";
        case IndexKind::Inverted:
            return "inverted";
    }
    return "kv";
}

static bool ParseKind(std::string_view json, IndexKind* out) {
    constexpr std::string_view key = "\"kind\"";
    auto pos = json.find(key);
    if (pos == std::string::npos) return false;
    pos += key.size();
    while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos]))) ++pos;
    if (pos >= json.size() || json[pos] != ':') return false;
    ++pos;
    while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos]))) ++pos;
    if (pos >= json.size() || json[pos] != '"') return false;
    ++pos;
    auto endq = json.find('"', pos);
    if (endq == std::string::npos) return false;
    std::string_view val = json.substr(pos, endq - pos);
    if (val == "kv") {
        *out = IndexKind::KV;
        return true;
    }
    if (val == "inverted") {
        *out = IndexKind::Inverted;
        return true;
    }
    return false;
}

static bool ParseU64Field(std::string_view json, std::string_view field, std::uint64_t* out) {
    const std::string needle = std::string("\"") + std::string(field) + "\"";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return false;
    pos += needle.size();
    while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos]))) ++pos;
    if (pos >= json.size() || json[pos] != ':') return false;
    ++pos;
    while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos]))) ++pos;
    std::string rest(json.substr(pos));
    char* endptr = nullptr;
    unsigned long long v = std::strtoull(rest.c_str(), &endptr, 10);
    if (endptr == rest.c_str()) return false;
    *out = static_cast<std::uint64_t>(v);
    return true;
}

bool LoadIndexMeta(const std::string& json, IndexMeta* out, std::string* err) {
    if (!out) return false;
    IndexKind kind = IndexKind::KV;
    if (!ParseKind(json, &kind)) {
        if (err) *err = "LoadIndexMeta: missing or invalid \"kind\"";
        return false;
    }
    std::uint64_t ih = 0, dh = 0, ph = 0;
    if (!ParseU64Field(json, "index_hdr_off", &ih)) {
        if (err) *err = "LoadIndexMeta: missing index_hdr_off";
        return false;
    }
    if (!ParseU64Field(json, "docs_hdr_off", &dh)) {
        if (err) *err = "LoadIndexMeta: missing docs_hdr_off";
        return false;
    }
    if (!ParseU64Field(json, "posting_hdr_off", &ph)) {
        if (err) *err = "LoadIndexMeta: missing posting_hdr_off";
        return false;
    }
    out->kind            = kind;
    out->index_hdr_off   = ih;
    out->docs_hdr_off    = dh;
    out->posting_hdr_off = ph;
    return true;
}

std::string SerializeIndexMeta(const IndexMeta& m) {
    std::ostringstream os;
    os << "{\n"
       << "  \"kind\": \"" << IndexKindName(m.kind) << "\",\n"
       << "  \"index_hdr_off\": " << m.index_hdr_off << ",\n"
       << "  \"docs_hdr_off\": " << m.docs_hdr_off << ",\n"
       << "  \"posting_hdr_off\": " << m.posting_hdr_off << "\n"
       << "}\n";
    return os.str();
}

}  // namespace db
}  // namespace yikv
