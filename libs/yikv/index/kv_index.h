#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "container/concurrent_hashmap.h"
#include "index/doc.h"
#include "index/index.h"

namespace yikv {
namespace index {

// Key-value index backed by container::ConcurrentHashMap.
//
// The map stores: pk_string -> doc_slot_offset (arena offset of the Doc).
// All data (Doc layout + map nodes) lives in the Allocator arena and
// can be recovered from it after a crash/restart.
//
// Recovery offsets:
//   index_hdr_off : arena offset of the IndexHeader (holds next_doc_id).
//   docs_hdr_off  : arena offset of the ConcurrentHashMap head region
//                   (MxHeadTablePreamble + bucket table; see concurrent_hashmap.h).
// Pass both as 0 to create a fresh index.
//
// On-disk note: docs_hdr_off is **not** compatible with older yikv indexes that
// stored a HashMap HmRoot offset; reopening such an index requires rebuild/migration.
//
// When docs_hdr_off == 0 (new index), initial_docs_bucket_bits configures the
// map's initial bucket table (2^n buckets). chm_stripe_shift sets
// num_stripes = 1 << chm_stripe_shift (must match on recovery).
//
// Thread-safety (KV only — InvertedIndex posting bitmaps are still SWMR):
//   - Get / BatchGet / Size: concurrent with each other and with Put / BatchPut
//     on distinct primary keys.
//   - Put / BatchPut: safe concurrently when keys are distinct (insert-only
//     contract). Same-key concurrent Put can leak the overwritten slot.
//   - Upsert / BatchUpsert / Delete: serialized with each other via write_mx_;
//     they may run concurrently with Get if the app tolerates visibility races
//     (e.g. Get for a key while Delete(pk) runs may observe missing row).
//   - NewDoc: concurrent-safe doc_id via atomic header; use
//     AllocatorMode::Concurrent on FtAllocator for concurrent arena allocation.
//   - Publish: may be called concurrently; FtAllocator reclaims under per-class
//     locks in Concurrent mode.

class KVIndex : public Index {
public:
    explicit KVIndex(alloc::Allocator*   alloc,
                     const schema::Schema* schema,
                     uint64_t index_hdr_off         = 0,
                     uint64_t docs_hdr_off          = 0,
                     uint32_t initial_docs_bucket_bits = 15,
                     uint8_t  chm_stripe_shift      = 6);

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

    // HashMap-only bulk mode had eliminated CoW directory churn during offline import.
    // ConcurrentHashMap has no equivalent staging path; this is a no-op (call is safe).
    void EnableBulkMode();

    // Fill *out with an attached Doc if pk exists; returns false otherwise.
    bool Get(std::string_view pk, Doc* out) const;
    void BatchGet(const std::vector<std::string_view>& pks,
                  std::vector<Doc>* out) const;

    virtual bool Delete(std::string_view pk);

    // Reclaim allocator delayed frees / flush thread-local caches (optional checkpoint).
    void Publish() override;

    // Offset of ConcurrentHashMap head region (persist as docs_hdr_off).
    uint64_t docs_root_offset()  const noexcept;
    uint64_t index_hdr_offset()  const noexcept { return index_hdr_off_; }
    size_t   Size()              const noexcept;

protected:
    // Persisted counter so doc_id survives restarts (next_doc_id updated atomically).
    struct IndexHeader {
        uint32_t next_doc_id;
        uint32_t reserved;
    };

    std::string ExtractPk(const Doc& doc) const;
    uint32_t    NextDocId();

    uint64_t                index_hdr_off_;
    // Serializes Upsert / BatchUpsert / Delete paths that read+Retire+mutate map.
    mutable std::mutex      write_mx_;
    // Value (doc arena offset) is stored inline in HmBlobEntry::val_off — no
    // extra arena allocation per entry, saving ~32 bytes × N entries.
    std::unique_ptr<container::ConcurrentHashMap<
        std::string, uint64_t,
        std::hash<std::string>, std::equal_to<std::string>,
        container::DefaultCodec<std::string>,
        container::InlineU64Codec>> docs_;
};

}  // namespace index
}  // namespace yikv
