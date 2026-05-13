#include "src/index/kv_index.h"

namespace yikv {
namespace index {
namespace {

inline uint32_t atomic_fetch_add_u32_relax(uint32_t* p, uint32_t delta) {
    return __atomic_fetch_add(p, delta, __ATOMIC_RELAXED);
}

}  // namespace

KVIndex::KVIndex(alloc::Allocator*   alloc,
                 const schema::Schema* schema,
                 uint64_t index_hdr_off,
                 uint64_t docs_hdr_off,
                 uint32_t initial_docs_bucket_bits,
                 uint8_t  chm_stripe_shift)
    : Index(alloc, schema) {
    docs_ = std::make_unique<container::ConcurrentHashMap<
        std::string, uint64_t,
        std::hash<std::string>, std::equal_to<std::string>,
        container::DefaultCodec<std::string>,
        container::InlineU64Codec>>(
        alloc_, docs_hdr_off, initial_docs_bucket_bits, chm_stripe_shift);

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
    return atomic_fetch_add_u32_relax(&hdr->next_doc_id, 1u);
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
    docs_->put(pk, doc->slot_offset());
}

void KVIndex::BatchPut(const std::vector<Doc*>& docs) {
    constexpr size_t kFlushEvery = 50'000;
    for (size_t i = 0; i < docs.size(); ++i) {
        docs_->put(ExtractPk(*docs[i]), docs[i]->slot_offset());
        if ((i + 1) % kFlushEvery == 0) {
            alloc_->FlushTlc();
            alloc_->ReclaimExpired();
        }
    }
    alloc_->FlushTlc();
    alloc_->ReclaimExpired();
}

void KVIndex::EnableBulkMode() {
    // ConcurrentHashMap has no HashMap-style bulk/CoW staging.
}

void KVIndex::Upsert(Doc* doc) {
    std::string pk = ExtractPk(*doc);

    std::lock_guard<std::mutex> lock(write_mx_);
    uint64_t old_off = 0;
    if (docs_->get(pk, old_off)) {
        Doc(alloc_, old_off).Retire();
    }

    docs_->put(pk, doc->slot_offset());
}

void KVIndex::BatchUpsert(const std::vector<Doc*>& docs) {
    std::lock_guard<std::mutex> lock(write_mx_);
    for (Doc* d : docs) {
        std::string pk = ExtractPk(*d);
        uint64_t old_off = 0;
        if (docs_->get(pk, old_off)) {
            Doc(alloc_, old_off).Retire();
        }
        docs_->put(pk, d->slot_offset());
    }
}

bool KVIndex::Get(std::string_view pk, Doc* out) const {
    if (!out) return false;
    uint64_t off = 0;
    if (!docs_->get(std::string(pk), off)) return false;
    *out = Doc(alloc_, off);
    return true;
}

void KVIndex::BatchGet(const std::vector<std::string_view>& pks,
                       std::vector<Doc>* out) const {
    if (!out) return;
    out->reserve(pks.size());
    for (const auto& pk : pks) {
        uint64_t off = 0;
        if (docs_->get(std::string(pk), off)) {
            out->emplace_back(alloc_, off);
        }
    }
}

bool KVIndex::Delete(std::string_view pk) {
    std::string k(pk);
    std::lock_guard<std::mutex> lock(write_mx_);
    uint64_t old_off = 0;
    if (docs_->get(k, old_off)) {
        Doc(alloc_, old_off).Retire();
    }
    return docs_->erase(k);
}

void KVIndex::Publish() {
    alloc_->FlushTlc();
    alloc_->ReclaimExpired();
}

uint64_t KVIndex::docs_root_offset() const noexcept {
    return docs_->head_region_offset();
}

size_t KVIndex::Size() const noexcept {
    return docs_->size();
}

}  // namespace index
}  // namespace yikv
