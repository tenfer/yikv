#include "src/index/inverted_index.h"

#include <algorithm>
#include <cctype>

namespace yikv {
namespace index {

InvertedIndex::InvertedIndex(alloc::Allocator*   alloc,
                             const schema::Schema* schema,
                             uint64_t index_hdr_off,
                             uint64_t docs_hdr_off,
                             uint64_t posting_hdr_off,
                             uint32_t initial_docs_bucket_bits,
                             uint8_t  chm_stripe_shift)
    : KVIndex(alloc, schema, index_hdr_off, docs_hdr_off,
              initial_docs_bucket_bits, chm_stripe_shift) {
    // Posting map: separate table; keep 2^15 default buckets (matches prior HashMap).
    postings_ = std::make_unique<container::ConcurrentHashMap<std::string, uint64_t>>(
        alloc_, posting_hdr_off, 15u, chm_stripe_shift);
}

// ── Text helpers ─────────────────────────────────────────────────────────────

std::string InvertedIndex::Normalize(std::string_view s) {
    std::string out(s);
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

std::vector<std::string> InvertedIndex::Tokenize(std::string_view text) {
    std::vector<std::string> tokens;
    size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && !std::isalnum(static_cast<unsigned char>(text[i]))) ++i;
        size_t start = i;
        while (i < text.size() &&  std::isalnum(static_cast<unsigned char>(text[i]))) ++i;
        if (i > start) {
            tokens.push_back(Normalize({text.data() + start, i - start}));
        }
    }
    return tokens;
}

std::string InvertedIndex::PostingKey(uint16_t field_id, std::string_view term) {
    return std::to_string(field_id) + "#" + Normalize(term);
}

// ── Posting list helpers ──────────────────────────────────────────────────────

void InvertedIndex::AddToPosting(uint16_t field_id, std::string_view term,
                                 uint32_t doc_id) {
    const std::string pkey = PostingKey(field_id, term);
    uint64_t off = 0;
    if (postings_->get(pkey, off)) {
        container::Bitmap bm(alloc_, off);
        bm.Add(doc_id);
    } else {
        container::Bitmap bm(alloc_, 0);
        bm.Add(doc_id);
        postings_->put(pkey, bm.root_offset());
    }
}

void InvertedIndex::RemoveFromPosting(uint16_t field_id, std::string_view term,
                                      uint32_t doc_id) {
    const std::string pkey = PostingKey(field_id, term);
    uint64_t off = 0;
    if (!postings_->get(pkey, off)) return;
    container::Bitmap bm(alloc_, off);
    bm.Remove(doc_id);
    if (bm.IsEmpty()) {
        postings_->erase(pkey);
    }
}

// ── Index / Deindex ───────────────────────────────────────────────────────────

void InvertedIndex::IndexDoc(const Doc& doc) {
    uint32_t doc_id = doc.doc_id();
    for (const auto& field : schema_->fields()) {
        if (!field->is_index) continue;
        uint16_t fid = field->field_id;
        switch (field->type) {
            case schema::DataType::String:
                for (const auto& tok : Tokenize(doc.get_string(fid))) {
                    AddToPosting(fid, tok, doc_id);
                }
                break;
            case schema::DataType::Int32:
                AddToPosting(fid, std::to_string(doc.get_int32(fid)), doc_id);
                break;
            case schema::DataType::Int64:
                AddToPosting(fid, std::to_string(doc.get_int64(fid)), doc_id);
                break;
            default:
                break;
        }
    }
}

void InvertedIndex::DeindexDoc(const Doc& doc) {
    uint32_t doc_id = doc.doc_id();
    for (const auto& field : schema_->fields()) {
        if (!field->is_index) continue;
        uint16_t fid = field->field_id;
        switch (field->type) {
            case schema::DataType::String:
                for (const auto& tok : Tokenize(doc.get_string(fid))) {
                    RemoveFromPosting(fid, tok, doc_id);
                }
                break;
            case schema::DataType::Int32:
                RemoveFromPosting(fid, std::to_string(doc.get_int32(fid)), doc_id);
                break;
            case schema::DataType::Int64:
                RemoveFromPosting(fid, std::to_string(doc.get_int64(fid)), doc_id);
                break;
            default:
                break;
        }
    }
}

// ── Overrides ─────────────────────────────────────────────────────────────────

void InvertedIndex::Put(Doc* doc) {
    // De-index the old doc before replacing it.
    Doc old_doc;
    if (Get(ExtractPk(*doc), &old_doc)) {
        DeindexDoc(old_doc);
    }
    KVIndex::Upsert(doc);  // retire old slot in docs_ if PK existed
    IndexDoc(*doc);
}

bool InvertedIndex::Delete(std::string_view pk) {
    Doc old_doc;
    if (Get(pk, &old_doc)) {
        DeindexDoc(old_doc);
    }
    return KVIndex::Delete(pk);  // erases docs_
}

// ── Query ─────────────────────────────────────────────────────────────────────

bool InvertedIndex::Query(uint16_t field_id, std::string_view term,
                          container::Bitmap* out) const {
    if (!out) return false;
    const std::string pkey = PostingKey(field_id, term);
    uint64_t off = 0;
    if (!postings_->get(pkey, off)) return false;
    *out = container::Bitmap(alloc_, off);
    return true;
}

container::Bitmap InvertedIndex::QueryAnd(uint16_t field_id,
                                          const std::vector<std::string>& terms) const {
    if (terms.empty()) return container::Bitmap(alloc_);
    std::vector<container::Bitmap> bitmaps;
    bitmaps.reserve(terms.size());
    for (const auto& term : terms) {
        container::Bitmap bm(alloc_, 0);
        if (!Query(field_id, term, &bm) || bm.IsEmpty()) {
            return container::Bitmap(alloc_);
        }
        bitmaps.push_back(std::move(bm));
    }
    std::sort(bitmaps.begin(), bitmaps.end(),
        [](const container::Bitmap& a, const container::Bitmap& b) {
            return a.Cardinality() < b.Cardinality();
        });
    container::Bitmap result(bitmaps[0]);
    for (size_t i = 1; i < bitmaps.size(); ++i) {
        result.AndWith(bitmaps[i]);
        if (result.IsEmpty()) return result;
    }
    return result;
}

container::Bitmap InvertedIndex::QueryOr(uint16_t field_id,
                                         const std::vector<std::string>& terms) const {
    container::Bitmap result(alloc_);
    for (const auto& term : terms) {
        container::Bitmap bm(alloc_, 0);
        if (Query(field_id, term, &bm)) result.OrWith(bm);
    }
    return result;
}

// ── Lifecycle ─────────────────────────────────────────────────────────────────

void InvertedIndex::Publish() {
    KVIndex::Publish();
}

uint64_t InvertedIndex::posting_root_offset() const noexcept {
    return postings_->head_region_offset();
}

}  // namespace index
}  // namespace yikv
