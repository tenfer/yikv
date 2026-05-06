#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "src/container/bitmap.h"
#include "src/container/hashmap.h"
#include "src/index/kv_index.h"

namespace yikv {
namespace index {

// Inverted index backed by KVIndex (doc store) plus a posting-list HashMap.
//
// Inherits all KV storage from KVIndex and overrides Put/Delete to
// maintain per-term Bitmap posting lists for every field that has
// is_index=true in the schema.
//
// Posting map: (field_id + "#" + normalized_term) -> bitmap_root_offset
//
// Recovery offsets (in addition to KVIndex's):
//   posting_hdr_off : arena offset of the postings HashMap root.

class InvertedIndex : public KVIndex {
public:
    explicit InvertedIndex(alloc::Allocator*   alloc,
                           const schema::Schema* schema,
                           uint64_t index_hdr_off   = 0,
                           uint64_t docs_hdr_off    = 0,
                           uint64_t posting_hdr_off = 0);

    // Upsert doc and update all is_index posting lists.
    void Put(Doc* doc) override;

    // Remove doc and clean up posting lists.
    bool Delete(std::string_view pk) override;

    // Single-term query: fills *out with the posting bitmap; returns false if absent.
    bool Query(uint16_t field_id, std::string_view term,
               container::Bitmap* out) const;

    // AND across terms: docs that contain all given terms in the field.
    container::Bitmap QueryAnd(uint16_t field_id,
                               const std::vector<std::string>& terms) const;

    // OR across terms: docs that contain any of the given terms in the field.
    container::Bitmap QueryOr(uint16_t field_id,
                              const std::vector<std::string>& terms) const;

    void     Publish() override;

    uint64_t posting_root_offset() const noexcept;

private:
    // Add doc_id to each is_index field's posting bitmap.
    void IndexDoc  (const Doc& doc);
    // Remove doc_id from each is_index field's posting bitmap.
    void DeindexDoc(const Doc& doc);

    void AddToPosting   (uint16_t field_id, std::string_view term, uint32_t doc_id);
    void RemoveFromPosting(uint16_t field_id, std::string_view term, uint32_t doc_id);

    static std::string              PostingKey(uint16_t field_id, std::string_view term);
    static std::vector<std::string> Tokenize  (std::string_view text);
    static std::string              Normalize  (std::string_view s);

    // field_id#term -> bitmap root_offset
    std::unique_ptr<container::HashMap<std::string, uint64_t>> postings_;
};

}  // namespace index
}  // namespace yikv
