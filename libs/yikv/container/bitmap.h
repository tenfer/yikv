#pragma once

#include "alloc/allocator.h"
#include "alloc/ft_allocator.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace yikv {
namespace container {

// ============================================================
// Persistent arena structures (no pointers / virtual addresses)
// ============================================================
namespace bm_detail {

constexpr uint32_t kBmMagic   = 0x504d4252u;  // "RBMP" little-endian
constexpr uint16_t kBmVersion = 1;

// Wire magic for Serialize / Deserialize (backward-compat export format).
constexpr uint32_t kWireMagic   = 0x314d4252u;  // "RBM1"
constexpr uint16_t kWireVersion = 1;

// One chunk index entry — 24 bytes, all offsets, no VAs.
struct alignas(8) BmChunkEntry {
    uint16_t hi;           // high-16 sort key (ascending)
    uint8_t  type;         // 0 = array, 1 = bitmap
    uint8_t  _pad;
    uint32_t cardinality;  // live element count in this chunk
    uint64_t payload_off;  // arena offset → payload bytes
    uint32_t payload_len;  // allocated payload capacity (bytes)
    uint32_t _pad2;
};
static_assert(sizeof(BmChunkEntry) == 24, "BmChunkEntry must be 24 B");
static_assert(alignof(BmChunkEntry) == 8);

// Root node — 32 bytes.
struct alignas(8) BmRoot {
    uint32_t magic;
    uint16_t version;
    uint16_t _pad;
    uint64_t cardinality;    // total element count
    uint32_t chunk_count;    // live entries in index
    uint32_t chunk_capacity; // allocated slots at chunks_off
    uint64_t chunks_off;     // arena offset → BmChunkEntry[]
};
static_assert(sizeof(BmRoot) == 32, "BmRoot must be 32 B");
static_assert(alignof(BmRoot) == 8);

}  // namespace bm_detail

// ============================================================
// Bitmap
//
// Arena-native roaring bitmap.  All data lives in the FtAllocator
// arena; the object is fully recoverable after mmap reopen without
// any serialization step.
//
// Persistence model
// -----------------
//   Construct with root_off == 0 on first use (allocates a new BmRoot).
//   Call root_offset() and store the result in the parent structure.
//   On process restart, mmap the same file, construct with the stored
//   root_off and the object is immediately ready.
//
// Thread-safety
// -------------
//   Single-writer / multiple-reader (SWMR).  Every mutating method
//   ends with PublishFence() so readers see a consistent state.
// ============================================================
class Bitmap {
public:
    using Allocator = yikv::alloc::Allocator;

    // Construct or recover.
    //   root_off == 0  → allocate a new BmRoot (first use).
    //   root_off != 0  → recover existing BmRoot (after mmap reopen).
    explicit Bitmap(Allocator* alloc, uint64_t root_off = 0);

    // Deep copy — creates an independent BmRoot in the same arena.
    Bitmap(const Bitmap& other);
    Bitmap& operator=(const Bitmap& other);

    Bitmap(Bitmap&&) noexcept;
    Bitmap& operator=(Bitmap&&) noexcept;

    ~Bitmap() = default;

    // Arena offset of BmRoot.  Store this in the parent structure for recovery.
    uint64_t root_offset() const noexcept { return root_off_; }

    // Allocator used by this bitmap.
    Allocator* allocator() const noexcept { return alloc_; }

    // ---- Point operations ----
    void     Add(uint32_t value);
    void     Remove(uint32_t value);
    bool     Contains(uint32_t value) const noexcept;
    uint64_t Cardinality() const noexcept;
    bool     IsEmpty() const noexcept;

    // ---- Bulk insert ----
    // sorted_values must be sorted ascending; duplicates are silently ignored.
    void BulkAdd(const uint32_t* sorted_values, size_t count);

    // ---- In-place set algebra (SWMR-safe after PublishFence) ----
    void OrWith(const Bitmap& other);
    void AndWith(const Bitmap& other);

    // ---- Value-returning set algebra (new Bitmap in same arena) ----
    Bitmap Or    (const Bitmap& other) const;
    Bitmap And   (const Bitmap& other) const;
    Bitmap Xor   (const Bitmap& other) const;
    Bitmap AndNot(const Bitmap& other) const;

    // Backward-compatible aliases.
    Bitmap Union       (const Bitmap& other) const { return Or(other);     }
    Bitmap Intersection(const Bitmap& other) const { return And(other);    }
    Bitmap Difference  (const Bitmap& other) const { return AndNot(other); }

    // ---- Iteration ----
    template <class Fn>
    void ForEach(Fn&& fn) const;

    // ---- Memory estimate ----
    size_t EstimatedBytes() const noexcept;

    // ---- Portable export / import (optional migration tool) ----
    std::vector<uint8_t>  Serialize() const;
    static Bitmap Deserialize(Allocator* alloc, const uint8_t* data, size_t size);
    // Convenience overload — creates an anonymous arena internally.
    static Bitmap Deserialize(const uint8_t* data, size_t size);

private:
    static constexpr uint32_t kBitmapWords            = 1024;  // 65536 bits = 8 KB
    static constexpr uint32_t kArrayToBitmapThreshold = 4096;
    static constexpr uint32_t kBitmapToArrayThreshold = 2048;
    static constexpr uint32_t kIndexInitCap           = 8;
    static constexpr uint32_t kArrayInitCap           = 16;    // uint16_t slots

    Allocator*                   alloc_      = nullptr;
    void*                        base_       = nullptr;
    uint64_t                     root_off_   = 0;
    // Non-null only when this Bitmap owns its arena (anonymous Deserialize).
    std::unique_ptr<yikv::alloc::FtAllocator> owned_alloc_;

    // ---- Arena helpers ----
    template <class T>       T* at(uint64_t off)       noexcept;
    template <class T> const T* at(uint64_t off) const noexcept;
    uint64_t off_of(const void* p) const noexcept;

    bm_detail::BmRoot*             root()  noexcept;
    const bm_detail::BmRoot*       root()  const noexcept;
    bm_detail::BmChunkEntry*       idx()   noexcept;
    const bm_detail::BmChunkEntry* idx()   const noexcept;

    // ---- Payload access ----
    uint16_t*       arr(const bm_detail::BmChunkEntry& e)       noexcept;
    const uint16_t* arr(const bm_detail::BmChunkEntry& e) const noexcept;
    uint64_t*       bmp(const bm_detail::BmChunkEntry& e)       noexcept;
    const uint64_t* bmp(const bm_detail::BmChunkEntry& e) const noexcept;

    // ---- Chunk lookup / management ----
    uint32_t                 chunk_pos(uint16_t hi)  const noexcept;
    bm_detail::BmChunkEntry* find_chunk(uint16_t hi) noexcept;
    const bm_detail::BmChunkEntry* find_chunk(uint16_t hi) const noexcept;
    bm_detail::BmChunkEntry* get_or_create(uint16_t hi);
    void                     erase_chunk(uint32_t pos);
    void                     ensure_index_cap(uint32_t needed);

    // ---- Mode conversion ----
    void grow_array(bm_detail::BmChunkEntry& e, uint32_t new_cap_elements);
    void to_bitmap(bm_detail::BmChunkEntry& e);
    void to_array(bm_detail::BmChunkEntry& e);

    // ---- Set-op helpers ----
    void     chunk_to_words(const bm_detail::BmChunkEntry& e,
                             uint64_t (&w)[kBitmapWords]) const noexcept;
    uint32_t popcount_words(const uint64_t (&w)[kBitmapWords]) noexcept;
    void     build_from_words(bm_detail::BmChunkEntry& e,
                              const uint64_t (&w)[kBitmapWords], uint32_t card);

    // Copy one chunk's payload from src_bm into a new entry at the end of this.
    void copy_append_chunk(uint16_t hi,
                           const bm_detail::BmChunkEntry& src,
                           const Bitmap& src_bm);

    void xor_with(const Bitmap& other);
    void and_not_with(const Bitmap& other);

};

// ---- ForEach (inline so the lambda is fully inlinable) ----
template <class Fn>
void Bitmap::ForEach(Fn&& fn) const {
    const auto* r = root();
    if (r->chunk_count == 0) return;
    const auto* entries = idx();
    for (uint32_t ci = 0; ci < r->chunk_count; ++ci) {
        const auto&    e    = entries[ci];
        const uint32_t hi32 = static_cast<uint32_t>(e.hi) << 16;
        if (e.type == 0) {
            const uint16_t* a = arr(e);
            for (uint32_t i = 0; i < e.cardinality; ++i)
                fn(hi32 | static_cast<uint32_t>(a[i]));
        } else {
            const uint64_t* b = bmp(e);
            for (uint32_t wi = 0; wi < kBitmapWords; ++wi) {
                uint64_t word = b[wi];
                while (word) {
                    uint32_t bit = static_cast<uint32_t>(__builtin_ctzll(word));
                    fn(hi32 | (wi << 6) | bit);
                    word &= word - 1;
                }
            }
        }
    }
}

// ---- Arena helpers (inline) ----
template <class T>
T* Bitmap::at(uint64_t off) noexcept {
    return reinterpret_cast<T*>(static_cast<char*>(base_) + off);
}
template <class T>
const T* Bitmap::at(uint64_t off) const noexcept {
    return reinterpret_cast<const T*>(static_cast<const char*>(base_) + off);
}
inline uint64_t Bitmap::off_of(const void* p) const noexcept {
    return static_cast<uint64_t>(
        static_cast<const char*>(p) - static_cast<const char*>(base_));
}
inline bm_detail::BmRoot* Bitmap::root() noexcept {
    return at<bm_detail::BmRoot>(root_off_);
}
inline const bm_detail::BmRoot* Bitmap::root() const noexcept {
    return at<const bm_detail::BmRoot>(root_off_);
}
inline bm_detail::BmChunkEntry* Bitmap::idx() noexcept {
    return at<bm_detail::BmChunkEntry>(root()->chunks_off);
}
inline const bm_detail::BmChunkEntry* Bitmap::idx() const noexcept {
    return at<const bm_detail::BmChunkEntry>(root()->chunks_off);
}
inline uint16_t* Bitmap::arr(const bm_detail::BmChunkEntry& e) noexcept {
    return at<uint16_t>(e.payload_off);
}
inline const uint16_t* Bitmap::arr(const bm_detail::BmChunkEntry& e) const noexcept {
    return at<const uint16_t>(e.payload_off);
}
inline uint64_t* Bitmap::bmp(const bm_detail::BmChunkEntry& e) noexcept {
    return at<uint64_t>(e.payload_off);
}
inline const uint64_t* Bitmap::bmp(const bm_detail::BmChunkEntry& e) const noexcept {
    return at<const uint64_t>(e.payload_off);
}

}  // namespace container
}  // namespace yikv
