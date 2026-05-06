#include "src/schema/schema.h"

#include <cctype>
#include <sstream>
#include <stdexcept>
#include <unordered_set>

namespace yikv {
namespace schema {

// ─── DataType helpers ─────────────────────────────────────────────────────────

const char* DataTypeName(DataType t) {
    switch (t) {
        case DataType::Bool:    return "bool";
        case DataType::Int32:   return "int32";
        case DataType::Int64:   return "int64";
        case DataType::Float32: return "float32";
        case DataType::Float64: return "float64";
        case DataType::String:  return "string";
        case DataType::Bytes:   return "bytes";
    }
    return "unknown";
}

bool ParseDataType(std::string_view name, DataType* out) {
    if (name == "bool")    { *out = DataType::Bool;    return true; }
    if (name == "int32")   { *out = DataType::Int32;   return true; }
    if (name == "int64")   { *out = DataType::Int64;   return true; }
    if (name == "float" || name == "float32") { *out = DataType::Float32; return true; }
    if (name == "double"|| name == "float64") { *out = DataType::Float64; return true; }
    if (name == "string")  { *out = DataType::String;  return true; }
    if (name == "bytes")   { *out = DataType::Bytes;   return true; }
    return false;
}

// ─── IndexType helpers ────────────────────────────────────────────────────────

const char* IndexTypeName(IndexType t) {
    switch (t) {
        case IndexType::None:     return "none";
        case IndexType::KV:       return "kv";
        case IndexType::Inverted: return "inverted";
        case IndexType::Vector:   return "vector";
    }
    return "none";
}

bool ParseIndexType(std::string_view name, IndexType* out) {
    if (name == "none")     { *out = IndexType::None;     return true; }
    if (name == "kv")       { *out = IndexType::KV;       return true; }
    if (name == "inverted") { *out = IndexType::Inverted; return true; }
    if (name == "vector")   { *out = IndexType::Vector;   return true; }
    return false;
}

// ─── TableFormat helpers ─────────────────────────────────────────────────────

const char* TableFormatName(TableFormat f) {
    switch (f) {
        case TableFormat::Json:            return "json";
        case TableFormat::FixedRowBinary:  return "fixed_row_binary";
        case TableFormat::SparseRowBinary: return "sparse_row_binary";
    }
    return "sparse_row_binary";
}

bool ParseTableFormat(std::string_view name, TableFormat* out) {
    if (name == "json")               { *out = TableFormat::Json;            return true; }
    if (name == "fixed_row_binary")   { *out = TableFormat::FixedRowBinary;  return true; }
    if (name == "sparse_row_binary"
     || name == "spare_row_binary")   { *out = TableFormat::SparseRowBinary; return true; }
    return false;
}

// ─── Schema::RebuildMaps ─────────────────────────────────────────────────────

void Schema::RebuildMaps() {
    field_map_.clear();
    field_id_map_.clear();
    max_field_id_ = 0;
    for (const auto& fd : fields_) {
        field_map_[fd->name]    = fd.get();
        field_id_map_[fd->field_id] = fd.get();
        if (fd->field_id > max_field_id_) max_field_id_ = fd->field_id;
    }
}

const FieldDef* Schema::FindField(std::string_view name) const {
    auto it = field_map_.find(std::string(name));
    return it == field_map_.end() ? nullptr : it->second;
}

const FieldDef* Schema::FindFieldById(uint16_t id) const {
    auto it = field_id_map_.find(id);
    return it == field_id_map_.end() ? nullptr : it->second;
}

// ─── Compile ─────────────────────────────────────────────────────────────────

bool Schema::Compile(CompiledSchema* out, std::string* err) const {
    out->layouts.assign(fields_.size(), FieldLayout{});
    out->n_fixed = 0;
    out->n_var   = 0;

    // Verify pk field exists.
    bool found_pk = false;
    for (size_t i = 0; i < fields_.size(); ++i) {
        if (fields_[i]->name == pk_) {
            out->pk_field_id = static_cast<uint32_t>(i);
            found_pk = true;
            break;
        }
    }
    if (!found_pk) {
        if (err) *err = "Schema: pk '" + pk_ + "' not found in fields";
        return false;
    }

    std::unordered_set<std::string> seen_names;
    std::unordered_set<uint16_t>    seen_ids;
    uint32_t fixed_slot = 0;
    uint32_t var_slot   = 0;

    for (uint32_t i = 0; i < fields_.size(); ++i) {
        const FieldDef& f = *fields_[i];

        if (!seen_names.insert(f.name).second) {
            if (err) *err = "Schema: duplicate field name '" + f.name + "'";
            return false;
        }
        if (!seen_ids.insert(f.field_id).second) {
            if (err) *err = "Schema: duplicate field_id " + std::to_string(f.field_id);
            return false;
        }

        FieldLayout& fl  = out->layouts[i];
        fl.field_id       = f.field_id;
        fl.type           = f.type;
        fl.is_array       = f.is_array;
        fl.is_fixed_scalar = !f.is_array && IsFixedScalar(f.type);

        if (fl.is_fixed_scalar) {
            fl.file_fixed_off = fixed_slot * 8;
            fl.mem_chunk_idx  = fixed_slot / 8;
            fl.mem_chunk_off  = (fixed_slot % 8) * 8;
            ++fixed_slot;
            ++out->n_fixed;
        } else {
            fl.file_var_idx = var_slot;
            fl.mem_var_idx  = var_slot;
            if (f.is_array && IsFixedScalar(f.type)) {
                fl.elem_size = FixedTypeSize(f.type);
            }
            ++var_slot;
            ++out->n_var;
        }
    }

    out->fixed_payload_bytes  = out->n_fixed * 8;
    out->mem_n_fixed_chunks   = out->n_fixed ? (out->n_fixed + 7) / 8 : 0;
    out->mem_n_var_slots      = out->n_var;
    return true;
}

// ─── Minimal JSON scanner ─────────────────────────────────────────────────────

namespace {

struct JsonScanner {
    const char* p;
    const char* end;
    std::string err;

    void skip_ws() {
        while (p < end) {
            char c = *p;
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') { ++p; continue; }
            if (c == '/' && p + 1 < end && p[1] == '/') {
                while (p < end && *p != '\n') ++p;
                continue;
            }
            break;
        }
    }

    bool expect(char c) {
        skip_ws();
        if (p >= end || *p != c) { err = std::string("expected '") + c + "'"; return false; }
        ++p;
        return true;
    }

    bool parse_string(std::string* out) {
        skip_ws();
        if (p >= end || *p != '"') { err = "expected string"; return false; }
        ++p;
        out->clear();
        while (p < end && *p != '"') {
            char c = *p++;
            if (c == '\\' && p < end) {
                char nc = *p++;
                switch (nc) {
                    case '"':  out->push_back('"');  break;
                    case '\\': out->push_back('\\'); break;
                    case 'n':  out->push_back('\n'); break;
                    case 't':  out->push_back('\t'); break;
                    case 'r':  out->push_back('\r'); break;
                    default:   out->push_back(nc);   break;
                }
            } else {
                out->push_back(c);
            }
        }
        if (p >= end) { err = "unterminated string"; return false; }
        ++p;
        return true;
    }

    bool parse_bool(bool* out) {
        skip_ws();
        if (p + 4 <= end && std::string_view(p, 4) == "true")  { *out = true;  p += 4; return true; }
        if (p + 5 <= end && std::string_view(p, 5) == "false") { *out = false; p += 5; return true; }
        err = "expected true/false";
        return false;
    }

    bool parse_int64(int64_t* out) {
        skip_ws();
        if (p >= end) { err = "expected integer"; return false; }
        bool neg = false;
        if (*p == '-') { neg = true; ++p; }
        if (p >= end || !std::isdigit(*p)) { err = "expected digit"; return false; }
        *out = 0;
        while (p < end && std::isdigit(*p)) {
            *out = *out * 10 + (*p++ - '0');
        }
        if (neg) *out = -*out;
        return true;
    }

    // Skip any JSON value (string, bool, number, object, array).
    bool skip_value() {
        skip_ws();
        if (p >= end) { err = "unexpected end"; return false; }
        if (*p == '"') {
            std::string tmp;
            return parse_string(&tmp);
        }
        if (*p == 't' || *p == 'f') {
            bool tmp;
            return parse_bool(&tmp);
        }
        if (*p == '{') {
            ++p;
            bool first = true;
            while (true) {
                skip_ws();
                if (p < end && *p == '}') { ++p; return true; }
                if (!first && !expect(',')) return false;
                first = false;
                std::string k;
                if (!parse_string(&k)) return false;
                if (!expect(':')) return false;
                if (!skip_value()) return false;
            }
        }
        if (*p == '[') {
            ++p;
            bool first = true;
            while (true) {
                skip_ws();
                if (p < end && *p == ']') { ++p; return true; }
                if (!first && !expect(',')) return false;
                first = false;
                if (!skip_value()) return false;
            }
        }
        // number
        if (*p == '-' || std::isdigit(*p)) {
            int64_t tmp;
            return parse_int64(&tmp);
        }
        err = "unexpected token";
        return false;
    }
};

}  // namespace

// ─── Schema::LoadJson ────────────────────────────────────────────────────────

bool Schema::LoadJson(const std::string& json, std::string* err) {
    JsonScanner s{json.data(), json.data() + json.size(), {}};
    auto fail = [&](const std::string& msg) -> bool {
        if (err) *err = "Schema::LoadJson: " + msg + ": " + s.err;
        return false;
    };

    if (!s.expect('{')) return fail("opening brace");

    table_name_.clear();
    version_  = 0;
    format_   = TableFormat::SparseRowBinary;
    pk_.clear();
    fields_.clear();

    bool first = true;
    while (true) {
        s.skip_ws();
        if (s.p < s.end && *s.p == '}') { ++s.p; break; }
        if (!first && !s.expect(',')) return fail("comma");
        first = false;

        std::string key;
        if (!s.parse_string(&key)) return fail("key");
        if (!s.expect(':')) return fail("colon");

        if (key == "table_name") {
            if (!s.parse_string(&table_name_)) return fail("table_name");
        } else if (key == "schema_version" || key == "version") {
            if (!s.parse_int64(&version_)) return fail("schema_version");
        } else if (key == "table_format") {
            std::string fmt_str;
            if (!s.parse_string(&fmt_str)) return fail("table_format");
            if (!ParseTableFormat(fmt_str, &format_)) {
                if (err) *err = "Schema::LoadJson: unknown table_format '" + fmt_str + "'";
                return false;
            }
        } else if (key == "pk") {
            if (!s.parse_string(&pk_)) return fail("pk");
        } else if (key == "fields") {
            if (!s.expect('[')) return fail("fields array open");
            bool first_field = true;
            while (true) {
                s.skip_ws();
                if (s.p < s.end && *s.p == ']') { ++s.p; break; }
                if (!first_field && !s.expect(',')) return fail("field comma");
                first_field = false;

                if (!s.expect('{')) return fail("field object open");
                auto fd  = std::make_unique<FieldDef>();
                bool first_mem = true;
                while (true) {
                    s.skip_ws();
                    if (s.p < s.end && *s.p == '}') { ++s.p; break; }
                    if (!first_mem && !s.expect(',')) return fail("field member comma");
                    first_mem = false;

                    std::string fkey;
                    if (!s.parse_string(&fkey)) return fail("field member key");
                    if (!s.expect(':')) return fail("field member colon");

                    if (fkey == "name") {
                        if (!s.parse_string(&fd->name)) return fail("name");
                    } else if (fkey == "data_type" || fkey == "type") {
                        std::string ts;
                        if (!s.parse_string(&ts)) return fail("data_type");
                        if (!ParseDataType(ts, &fd->type)) {
                            if (err) *err = "Schema::LoadJson: unknown data_type '" + ts + "'";
                            return false;
                        }
                    } else if (fkey == "is_array" || fkey == "repeated") {
                        if (!s.parse_bool(&fd->is_array)) return fail("is_array");
                    } else if (fkey == "nullable") {
                        if (!s.parse_bool(&fd->nullable)) return fail("nullable");
                    } else if (fkey == "is_pk") {
                        if (!s.parse_bool(&fd->is_pk)) return fail("is_pk");
                        if (fd->is_pk) pk_ = fd->name;
                    } else if (fkey == "is_index") {
                        if (!s.parse_bool(&fd->is_index)) return fail("is_index");
                    } else if (fkey == "field_id") {
                        int64_t fid;
                        if (!s.parse_int64(&fid)) return fail("field_id");
                        fd->field_id = static_cast<uint16_t>(fid);
                    } else if (fkey == "field_offset" || fkey == "fixed_off") {
                        int64_t off;
                        if (!s.parse_int64(&off)) return fail("fixed_off");
                        fd->fixed_off = static_cast<uint32_t>(off);
                    } else if (fkey == "index_type") {
                        std::string it_str;
                        if (!s.parse_string(&it_str)) return fail("index_type");
                        if (!ParseIndexType(it_str, &fd->index_type)) {
                            if (err) *err = "Schema::LoadJson: unknown index_type '" + it_str + "'";
                            return false;
                        }
                    } else {
                        if (!s.skip_value()) return fail("unknown field member value");
                    }
                }
                // Auto-assign field_id if absent.
                if (fd->field_id == 0 && !fields_.empty()) {
                    fd->field_id = max_field_id_ + 1;
                }
                if (fd->field_id > max_field_id_) max_field_id_ = fd->field_id;
                fields_.push_back(std::move(fd));
            }
        } else {
            if (!s.skip_value()) return fail("unknown top-level value");
        }
    }

    if (pk_.empty()) {
        if (err) *err = "Schema::LoadJson: 'pk' is required";
        return false;
    }

    RebuildMaps();
    return true;
}

// ─── Schema::ToJson ──────────────────────────────────────────────────────────

std::string Schema::ToJson() const {
    std::ostringstream os;
    os << "{";
    os << "\"table_name\":\""    << table_name_            << "\",";
    os << "\"schema_version\":"  << version_               << ",";
    os << "\"table_format\":\""  << TableFormatName(format_) << "\",";
    os << "\"pk\":\""           << pk_                     << "\",";
    os << "\"fields\":[";
    for (size_t i = 0; i < fields_.size(); ++i) {
        if (i) os << ",";
        const auto& f = *fields_[i];
        os << "{";
        os << "\"name\":\""       << f.name                   << "\",";
        os << "\"data_type\":\""  << DataTypeName(f.type)     << "\",";
        os << "\"is_array\":"     << (f.is_array ? "true" : "false") << ",";
        os << "\"nullable\":"     << (f.nullable ? "true" : "false") << ",";
        os << "\"is_pk\":"        << (f.is_pk    ? "true" : "false") << ",";
        os << "\"is_index\":"     << (f.is_index ? "true" : "false") << ",";
        os << "\"field_id\":"     << f.field_id               << ",";
        os << "\"fixed_off\":"    << f.fixed_off               << ",";
        os << "\"index_type\":\"" << IndexTypeName(f.index_type) << "\"";
        os << "}";
    }
    os << "]}";
    return os.str();
}

}  // namespace schema
}  // namespace yikv
