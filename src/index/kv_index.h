#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "src/container/hashmap.h"
#include "src/index/doc.h"
#include "src/index/index.h"

namespace yikv {
namespace index {

// Key-value index backed by container::HashMap.
//
// The map stores: pk_string -> doc_slot_offset (arena offset of the Doc).
// All data (Doc layout + HashMap nodes) lives in the Allocator arena and
// can be recovered from it after a crash/restart.
//
// Recovery offsets:
//   index_hdr_off : arena offset of the IndexHeader (holds next_doc_id).
//   docs_hdr_off  : arena offset of the HashMap root node.
// Pass both as 0 to create a fresh index.
//
// When docs_hdr_off == 0 (new index), initial_docs_bucket_bits configures the
// HashMap's initial bucket table (2^n buckets; rehash roughly at 2*n entries
// due to kLoadFactor). Larger values reduce rehash churn on bulk inserts at
// the cost of a slightly larger root; ignored on recovery (non-zero hdr).

class KVIndex : public Index {
public:
    explicit KVIndex(alloc::Allocator*   alloc,
                     const schema::Schema* schema,
                     uint64_t index_hdr_off        = 0,
                     uint64_t docs_hdr_off         = 0,
                     uint32_t initial_docs_bucket_bits = 15);

    ~KVIndex() override = default;

    // Allocate a new Doc in the arena with a fresh sequential doc_id.
    Doc NewDoc();

    // Insert-only: *doc's primary key must not already exist. No map read or
    // retirement of a prior slot (call Upsert for replace semantics).
    virtual void Put(Doc* doc);
    void BatchPut(const std::vector<Doc*>& docs);

    // Replace-or-insert: retires any existing row for the same PK, then stores *doc.
    void Upsert(Doc* doc);
    void BatchUpsert(const std::vector<Doc*>& docs);

    // Enable in-place bulk-insert mode on the underlying HashMap.
    // ONLY call when there are no concurrent readers (e.g., offline import tool).
    // Eliminates CoW directory overhead per Put/BatchPut.
    void EnableBulkMode();

    // Fill *out with an attached Doc if pk exists; returns false otherwise.
    bool Get(std::string_view pk, Doc* out) const;
    void BatchGet(const std::vector<std::string_view>& pks,
                  std::vector<Doc>* out) const;

    virtual bool Delete(std::string_view pk);

    virtual void Publish();

    uint64_t docs_root_offset()  const noexcept;
    uint64_t index_hdr_offset()  const noexcept { return index_hdr_off_; }
    size_t   Size()              const noexcept;

protected:
    // Persisted counter so doc_id survives restarts.
    struct IndexHeader {
        uint32_t next_doc_id;
        uint32_t reserved;
    };

    std::string ExtractPk(const Doc& doc) const;
    uint32_t    NextDocId();

    uint64_t                index_hdr_off_;
    // Value (doc arena offset) is stored inline in HmBlobEntry::val_off — no
    // extra arena allocation per entry, saving ~32 bytes × N entries.
    std::unique_ptr<container::HashMap<
        std::string, uint64_t,
        std::hash<std::string>, std::equal_to<std::string>,
        container::DefaultCodec<std::string>,
        container::InlineU64Codec>> docs_;
};

}  // namespace index
}  // namespace yikv
