#include "src/index/kv_index.h"

namespace yikv {
namespace index {

KVIndex::KVIndex(alloc::Allocator*   alloc,
                 const schema::Schema* schema,
                 uint64_t index_hdr_off,
                 uint64_t docs_hdr_off,
                 uint32_t initial_docs_bucket_bits)
    : Index(alloc, schema) {
    docs_ = std::make_unique<container::HashMap<std::string, uint64_t>>(
        alloc_, docs_hdr_off, initial_docs_bucket_bits);

    if (index_hdr_off != 0) {
        index_hdr_off_ = index_hdr_off;
    } else {
        auto* hdr = reinterpret_cast<IndexHeader*>(alloc_->Malloc(sizeof(IndexHeader)));
        hdr->next_doc_id = 1;
        hdr->reserved    = 0;
        index_hdr_off_   = alloc_->PtrToOffset(hdr);
    }
}

uint32_t KVIndex::NextDocId() {
    auto* hdr = reinterpret_cast<IndexHeader*>(alloc_->OffsetToPtr(index_hdr_off_));
    return hdr->next_doc_id++;
}

Doc KVIndex::NewDoc() {
    uint32_t n_slots = static_cast<uint32_t>(schema_->MaxFieldId()) + 1;
    return Doc(alloc_, n_slots, NextDocId());
}

std::string KVIndex::ExtractPk(const Doc& doc) const {
    const schema::FieldDef* pk = schema_->FindField(schema_->pk());
    if (!pk) return {};
    uint32_t fid = pk->field_id;
    switch (pk->type) {
        case schema::DataType::Int32:  return std::to_string(doc.get_int32(fid));
        case schema::DataType::Int64:  return std::to_string(doc.get_int64(fid));
        case schema::DataType::String: return std::string(doc.get_string(fid));
        default:                       return {};
    }
}

void KVIndex::Put(Doc* doc) {
    std::string pk = ExtractPk(*doc);

    uint64_t old_off = 0;
    if (docs_->staged_get(pk, old_off)) {
        Doc(alloc_, old_off).Retire();
    }

    docs_->put(pk, doc->slot_offset());
    docs_->publish();
}

void KVIndex::BatchPut(const std::vector<Doc*>& docs) {
    for (Doc* d : docs) {
        std::string pk = ExtractPk(*d);
        uint64_t old_off = 0;
        if (docs_->staged_get(pk, old_off)) {
            Doc(alloc_, old_off).Retire();
        }
        docs_->put(pk, d->slot_offset());
    }
    docs_->publish();
}

bool KVIndex::Get(std::string_view pk, Doc* out) const {
    if (!out) return false;
    auto snap = docs_->acquire_snapshot();
    uint64_t off = 0;
    if (!snap.get(std::string(pk), off)) return false;
    *out = Doc(alloc_, off);
    return true;
}

void KVIndex::BatchGet(const std::vector<std::string_view>& pks,
                       std::vector<Doc>* out) const {
    if (!out) return;
    out->reserve(pks.size());
    auto snap = docs_->acquire_snapshot();
    for (const auto& pk : pks) {
        uint64_t off = 0;
        if (snap.get(std::string(pk), off)) {
            out->emplace_back(alloc_, off);
        }
    }
}

bool KVIndex::Delete(std::string_view pk) {
    std::string k(pk);
    uint64_t old_off = 0;
    if (docs_->staged_get(k, old_off)) {
        Doc(alloc_, old_off).Retire();
    }
    bool ok = docs_->erase(k);
    if (ok) docs_->publish();
    return ok;
}

void KVIndex::Publish() {
    docs_->publish();
}

uint64_t KVIndex::docs_root_offset() const noexcept {
    return docs_->root_offset();
}

size_t KVIndex::Size() const noexcept {
    return docs_->size();
}

}  // namespace index
}  // namespace yikv
