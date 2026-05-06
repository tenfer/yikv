#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// ============================================================
//  yikv::schema  —  unified schema definitions
//
//  Merges two previous schemas:
//    • Historical index-oriented schema (superseded; see yikv::index + schema JSON)
//    • yikv::DocSchema      (src/storage/schema/*)  – storage-oriented
//
//  Design goals
//  ─────────────
//  1. Single source of truth for field metadata.
//  2. Drives both the disk layer (FileDoc layout) and the memory layer
//     (arena documents: e.g. yikv::index::Doc / historical MemDoc / KVTable layout).
//  3. Carries index_type so upper layers (yikv::index) know how to build
//     KVIndex / InvertedIndex / VectorIndex for each field.
//  4. JSON round-trip with stable field_id anchoring.
// ============================================================

namespace yikv {
namespace schema {

// ─── DataType ─────────────────────────────────────────────────────────────────

enum class DataType : uint8_t {
    Bool    = 0,
    Int32   = 1,
    Int64   = 2,
    Float32 = 3,
    Float64 = 4,
    String  = 5,
    Bytes   = 6,
};

const char* DataTypeName(DataType t);
bool        ParseDataType(std::string_view name, DataType* out);

inline bool IsFixedScalar(DataType t) {
    switch (t) {
        case DataType::Bool:
        case DataType::Int32:
        case DataType::Int64:
        case DataType::Float32:
        case DataType::Float64:
            return true;
        default:
            return false;
    }
}

inline uint32_t FixedTypeSize(DataType t) {
    switch (t) {
        case DataType::Bool:    return 1;
        case DataType::Int32:   return 4;
        case DataType::Int64:   return 8;
        case DataType::Float32: return 4;
        case DataType::Float64: return 8;
        default:                return 0;
    }
}

// ─── IndexType ────────────────────────────────────────────────────────────────
//
// Indicates which (if any) in-memory index structure backs this field.
// The index layer (yikv::index) reads this to know what to build.

enum class IndexType : uint8_t {
    None     = 0,   // not indexed
    KV       = 1,   // exact-match, backed by container::HashMap
    Inverted = 2,   // term-based, backed by HashMap + Bitmap
    Vector   = 3,   // ANN, backed by VectorIndex (FAISS stub)
};

const char* IndexTypeName(IndexType t);
bool        ParseIndexType(std::string_view name, IndexType* out);

// ─── FieldDef ─────────────────────────────────────────────────────────────────

struct FieldDef {
    std::string name;
    DataType    type        = DataType::Int32;
    bool        is_array    = false;  // repeated field (vector of values)
    bool        nullable    = true;   // nullable / optional
    bool        is_pk       = false;  // primary key: value used as storage key
    // Explicit index participation flag:
    // - For Inverted index, effective only when is_index=true && index_type=Inverted.
    // - For other index types, current behavior is preserved.
    bool        is_index    = false;

    // Stable identity anchor (never reuse after removal for SparseRow compat).
    uint16_t    field_id    = 0;

    // FileDoc fixed-payload slot offset (FixedRow mode).
    uint32_t    fixed_off   = 0;

    // Index type: which yikv::index structure backs this field.
    IndexType   index_type  = IndexType::None;

    // FixedRow head slot size in bytes (for in-arena doc layouts such as index::Doc).
    uint32_t SlotBytes() const {
        if (is_array) return 4;
        switch (type) {
            case DataType::Int64:
            case DataType::Float64:
                return 8;
            default:
                return 4;
        }
    }
};

// ─── CompiledLayout ───────────────────────────────────────────────────────────
//
// Derived once from Schema::Compile(); describes per-field memory/disk offsets.

struct FieldLayout {
    uint32_t field_id         = 0;
    DataType type             = DataType::Int32;
    bool     is_array         = false;
    bool     is_fixed_scalar  = false;

    // FileDoc binary layout:
    uint32_t file_fixed_off   = 0;   // byte offset in fixed_payload (8-byte slot)
    uint32_t file_var_idx     = 0;   // index into var_offsets[] (var fields only)

    // Arena / MemDoc-style compiled layout (kv_table / index::Doc consumers):
    uint32_t mem_chunk_idx    = 0;   // FixedChunk index       (fixed scalar only)
    uint32_t mem_chunk_off    = 0;   // byte offset in chunk   (fixed scalar only)
    uint32_t mem_var_idx      = 0;   // VarSlot index          (var fields only)
    uint32_t elem_size        = 0;   // bytes per element (fixed-element arrays only)
};

struct CompiledSchema {
    uint32_t                 n_fixed              = 0;
    uint32_t                 n_var                = 0;
    uint32_t                 pk_field_id          = 0;
    uint32_t                 fixed_payload_bytes  = 0;   // n_fixed * 8
    uint32_t                 mem_n_fixed_chunks   = 0;
    uint32_t                 mem_n_var_slots      = 0;
    std::vector<FieldLayout> layouts;                    // indexed by field_id
};

// ─── TableFormat ──────────────────────────────────────────────────────────────

enum class TableFormat : uint8_t {
    Json            = 0,   // human-readable JSON (debug only)
    FixedRowBinary  = 1,   // fixed-width row, all fields present
    SparseRowBinary = 2,   // sparse; only non-zero fields stored
};

const char* TableFormatName(TableFormat f);
bool        ParseTableFormat(std::string_view name, TableFormat* out);

// ─── Schema ───────────────────────────────────────────────────────────────────
//
// Canonical schema object.  Owned by Table; shared via shared_ptr with
// service and index layers.
//
// JSON format (stable / canonical):
//   {
//     "table_name":    "user_profile",
//     "schema_version": 1,
//     "table_format":  "sparse_row_binary",
//     "pk":            "user_id",
//     "fields": [
//       {
//         "name":       "user_id",
//         "data_type":  "int64",
//         "is_array":   false,
//         "nullable":   false,
//         "is_pk":      true,
//         "is_index":   false,
//         "field_id":   1,
//         "index_type": "kv"
//       }, ...
//     ]
//   }
//
// Compatibility rules (SparseRowBinary):
//   ✅ append new field with a fresh (larger) field_id
//   ✅ rename field (field_id unchanged)
//   ✅ drop field (old data silently ignored by reader)
//   ❌ change data_type / is_array / field_id of existing field
//
// Compatibility rules (FixedRowBinary):
//   ✅ append fields at the end
//   ❌ insert / remove / reorder

class Schema {
public:
    Schema() = default;

    // Build from JSON string.  Returns false on parse error; sets *err.
    bool LoadJson(const std::string& json, std::string* err = nullptr);

    // Serialize to canonical JSON.
    std::string ToJson() const;

    // Compile layout information (idempotent after first call).
    // Returns false if schema is malformed; sets *err.
    bool Compile(CompiledSchema* out, std::string* err = nullptr) const;

    // Accessors.
    const std::string&                          table_name()    const { return table_name_; }
    int64_t                                     version()       const { return version_; }
    TableFormat                                 format()        const { return format_; }
    const std::string&                          pk()            const { return pk_; }
    const std::vector<std::unique_ptr<FieldDef>>& fields()      const { return fields_; }

    const FieldDef* FindField(std::string_view name) const;
    const FieldDef* FindFieldById(uint16_t id)       const;

    uint16_t MaxFieldId()  const { return max_field_id_; }

private:
    std::string                                      table_name_;
    int64_t                                          version_{0};
    TableFormat                                      format_{TableFormat::SparseRowBinary};
    std::string                                      pk_;
    std::vector<std::unique_ptr<FieldDef>>           fields_;
    std::unordered_map<std::string, const FieldDef*> field_map_;
    std::unordered_map<uint16_t, const FieldDef*>    field_id_map_;
    uint16_t                                         max_field_id_{0};

    void RebuildMaps();
};

}  // namespace schema
}  // namespace yikv
