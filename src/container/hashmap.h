#pragma once

// HashMap<K, V>
//
// Arena-native, SWMR hash map.  All data lives in the FtAllocator arena;
// the structure is fully recoverable after mmap reopen without serialization.
//
// Persistence model
// -----------------
//   Construct with root_off == 0 on first use.
//   Call root_offset() and store the result in the parent structure.
//   On restart, mmap the same file, construct with the stored root_off.
//
// Concurrency (single-writer / multi-reader)
// ------------------------------------------
//   Writer: put/erase only update staged_root_ (invisible to readers until
//   publish). Then publish() writes HmHeader.root_off for crash recovery,
//   emits PublishFence(), release-stores pub_root_, AdvanceEpoch(), moves
//   retired CoW nodes into timestamped batches, and inline-sweeps batches older
//   than alloc_->ReclaimDelayNs() (Immediate free) plus alloc_->ReclaimExpired()
//   for allocator Delayed blocks (time-based; see AllocatorOptions).
//   Readers: acquire_snapshot() (acquire-load of pub_root_) and read lock-free.
//
// Directory layout (3-level tree, all offsets in arena)
// ------------------------------------------------------
//   HmRoot → HmDirChunk[256] → HmSegment[64] → HmBlock chain → Entry[]
//
// Write amplification per put (default 4096 buckets)
//   HmBlock (~48 B) + HmSegment (512 B) + HmDirChunk (512 B) + HmRoot (2 KB)
//   ≈ 3 KB per mutation, regardless of map size.

#include "src/alloc/allocator.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace yikv {
namespace container {

using Allocator   = yikv::alloc::Allocator;
using FreeMode    = yikv::alloc::FreeMode;

// ============================================================
// Persistent arena structures (no pointers / virtual addresses)
// ============================================================
namespace hm_detail {

constexpr uint32_t kMagic         = 0x4d485948u;  // "HYMH"
constexpr uint16_t kVersion       = 1;
constexpr uint32_t kLocalBits     = 6;  // 64 buckets per segment
constexpr uint32_t kSegBits       = 6;  // 64 segments per chunk
constexpr uint32_t kBucketsPerSeg = 1u << kLocalBits;   // 64
constexpr uint32_t kSegsPerChunk  = 1u << kSegBits;      // 64
constexpr uint32_t kMaxDirChunks  = 256;
constexpr uint32_t kBlockCap      = 4;   // entries per HmBlock
constexpr uint32_t kLoadFactor    = 2;   // rehash when entries > buckets * factor

// Address helpers ────────────────────────────────────────────────────────
template <class T>
inline T* at(void* base, uint64_t off) noexcept {
    return reinterpret_cast<T*>(static_cast<char*>(base) + off);
}
template <class T>
inline const T* at(const void* base, uint64_t off) noexcept {
    return reinterpret_cast<const T*>(static_cast<const char*>(base) + off);
}
inline uint64_t off_of(const void* base, const void* ptr) noexcept {
    return static_cast<uint64_t>(
        static_cast<const char*>(ptr) - static_cast<const char*>(base));
}

// Level 2: 64 segment offsets  (512 B) ──────────────────────────────────
struct alignas(8) HmDirChunk {
    uint64_t seg_off[kSegsPerChunk];
};
static_assert(sizeof(HmDirChunk) == 512);

// Level 1: 64 bucket-chain offsets  (512 B) ─────────────────────────────
struct alignas(8) HmSegment {
    uint64_t bkt_off[kBucketsPerSeg];
};
static_assert(sizeof(HmSegment) == 512);

// Level 0: block header (entries follow immediately) ─────────────────────
struct alignas(8) HmBlock {
    uint16_t count;        // live entries in this block
    uint16_t capacity;     // max entries (= kBlockCap)
    uint32_t entry_bytes;  // sizeof(Entry)
    uint64_t next;         // arena offset of next block (0 = none)
};
static_assert(sizeof(HmBlock) == 16);

// Entry types ────────────────────────────────────────────────────────────

// Inline: for trivially-copyable K and V
template <class K, class V>
struct alignas(8) HmInlineEntry {
    uint64_t hash;
    K        key;
    V        val;
};

// Blob: variable-length key / value stored as arena offsets
struct alignas(8) HmBlobEntry {
    uint64_t hash;
    uint64_t key_off;
    uint32_t key_len;
    uint32_t val_len;
    uint64_t val_off;
};
static_assert(sizeof(HmBlobEntry) == 32);

// Compile-time selector
template <class K, class V>
using HmEntry = std::conditional_t<
    std::is_trivially_copyable_v<K> && std::is_trivially_copyable_v<V>,
    HmInlineEntry<K, V>,
    HmBlobEntry>;

// Stable header  (16 B, allocated once, offset never changes) ───────────
// Stores the arena offset of the current HmRoot; updated on every publish().
// root_offset() returns this block's offset so the parent can recover the
// map even after many puts (each of which COWs a new HmRoot).
struct alignas(8) HmHeader {
    uint32_t magic;
    uint16_t version;
    uint16_t _pad;
    uint64_t root_off;   // arena offset of the current HmRoot
};
static_assert(sizeof(HmHeader) == 16);

// Root block  (32-byte header + 256 × 8-byte chunk offsets = 2080 B) ────
struct alignas(8) HmRoot {
    uint64_t entry_count;
    uint64_t bucket_count;   // always a power of 2
    uint32_t bucket_bits;    // log2(bucket_count)
    uint32_t chunk_count;    // number of allocated HmDirChunk slots
    uint64_t chunk_off[kMaxDirChunks];  // arena offsets → HmDirChunk
};
static_assert(offsetof(HmRoot, chunk_off) == 24);

}  // namespace hm_detail

// ============================================================
// DefaultCodec<T>
//
// Encodes a value into the arena and decodes it back.
// Specialise for non-trivial types; the default handles
// any trivially-copyable T.
// ============================================================
template <class T>
struct DefaultCodec {
    static uint64_t encode(Allocator& a, const T& v) {
        void* m = a.Malloc(sizeof(T));
        std::memcpy(m, &v, sizeof(T));
        return hm_detail::off_of(a.BaseAddress(), m);
    }
    static uint32_t encoded_len(const T&) noexcept {
        return static_cast<uint32_t>(sizeof(T));
    }
    static T decode(const void* base, uint64_t off, uint32_t /*len*/) noexcept {
        T r;
        std::memcpy(&r, static_cast<const char*>(base) + off, sizeof(T));
        return r;
    }
    static void retire(Allocator& a, uint64_t off) {
        a.Free(static_cast<char*>(a.BaseAddress()) + off, FreeMode::Delayed);
    }
};

template <>
struct DefaultCodec<std::string> {
    static uint64_t encode(Allocator& a, const std::string& s) {
        std::size_t n = s.size();
        void* m = a.Malloc(n > 0 ? n : 1);
        if (n > 0) std::memcpy(m, s.data(), n);
        return hm_detail::off_of(a.BaseAddress(), m);
    }
    static uint32_t encoded_len(const std::string& s) noexcept {
        return static_cast<uint32_t>(s.size());
    }
    static std::string decode(const void* base, uint64_t off, uint32_t len) {
        return {static_cast<const char*>(base) + off, len};
    }
    static void retire(Allocator& a, uint64_t off) {
        a.Free(static_cast<char*>(a.BaseAddress()) + off, FreeMode::Delayed);
    }
};

// ============================================================
// HashMap
// ============================================================
template <
    class K,
    class V,
    class Hash   = std::hash<K>,
    class Eq     = std::equal_to<K>,
    class KCodec = DefaultCodec<K>,
    class VCodec = DefaultCodec<V>>
class HashMap {
    using Entry    = hm_detail::HmEntry<K, V>;
    using Block    = hm_detail::HmBlock;
    using Segment  = hm_detail::HmSegment;
    using DirChunk = hm_detail::HmDirChunk;
    using Root     = hm_detail::HmRoot;

    static constexpr bool kInline =
        std::is_trivially_copyable_v<K> && std::is_trivially_copyable_v<V>;

    // ── Arena helpers ─────────────────────────────────────────────────
    template <class T> T* at(uint64_t off) noexcept {
        return hm_detail::at<T>(base_, off);
    }
    template <class T> const T* at(uint64_t off) const noexcept {
        return hm_detail::at<T>(base_, off);
    }
    uint64_t off_of(const void* p) const noexcept {
        return hm_detail::off_of(base_, p);
    }

    // ── Block entry access ────────────────────────────────────────────
    static Entry* blk_entries(Block* b) noexcept {
        return reinterpret_cast<Entry*>(reinterpret_cast<char*>(b) + sizeof(Block));
    }
    static const Entry* blk_entries(const Block* b) noexcept {
        return reinterpret_cast<const Entry*>(
            reinterpret_cast<const char*>(b) + sizeof(Block));
    }
    static std::size_t blk_alloc_sz() noexcept {
        return sizeof(Block) + hm_detail::kBlockCap * sizeof(Entry);
    }

    // ── Entry helpers ─────────────────────────────────────────────────
    static uint64_t entry_hash(const Entry& e) noexcept {
        // Both inline and blob entries start with uint64_t hash.
        return *reinterpret_cast<const uint64_t*>(&e);
    }
    bool entry_matches(const Entry& e, const K& k, uint64_t h) const {
        if constexpr (kInline) {
            const auto& ie = static_cast<const hm_detail::HmInlineEntry<K, V>&>(e);
            return ie.hash == h && Eq{}(ie.key, k);
        } else {
            const auto& be = static_cast<const hm_detail::HmBlobEntry&>(e);
            return be.hash == h &&
                   Eq{}(KCodec::decode(base_, be.key_off, be.key_len), k);
        }
    }
    V entry_value(const Entry& e) const {
        if constexpr (kInline)
            return static_cast<const hm_detail::HmInlineEntry<K, V>&>(e).val;
        else {
            const auto& be = static_cast<const hm_detail::HmBlobEntry&>(e);
            return VCodec::decode(base_, be.val_off, be.val_len);
        }
    }
    K entry_key(const Entry& e) const {
        if constexpr (kInline)
            return static_cast<const hm_detail::HmInlineEntry<K, V>&>(e).key;
        else {
            const auto& be = static_cast<const hm_detail::HmBlobEntry&>(e);
            return KCodec::decode(base_, be.key_off, be.key_len);
        }
    }
    void make_entry(Entry& e, uint64_t h, const K& k, const V& v) {
        if constexpr (kInline) {
            auto& ie = static_cast<hm_detail::HmInlineEntry<K, V>&>(e);
            ie.hash = h; ie.key = k; ie.val = v;
        } else {
            auto& be = static_cast<hm_detail::HmBlobEntry&>(e);
            be.hash    = h;
            be.key_off = KCodec::encode(*alloc_, k);
            be.key_len = KCodec::encoded_len(k);
            be.val_off = VCodec::encode(*alloc_, v);
            be.val_len = VCodec::encoded_len(v);
        }
    }
    void retire_entry_blobs(const Entry& e) {
        if constexpr (!kInline) {
            const auto& be = static_cast<const hm_detail::HmBlobEntry&>(e);
            if (be.key_off) retire_.push_back(be.key_off);
            if (be.val_off) retire_.push_back(be.val_off);
        }
    }

    // ── Bucket index decomposition ────────────────────────────────────
    struct BktIdx { uint64_t chunk, seg, local; };
    BktIdx decompose(uint64_t h, const Root* r) const noexcept {
        uint64_t bidx = h & (r->bucket_count - 1);
        return {
            bidx >> (hm_detail::kLocalBits + hm_detail::kSegBits),
            (bidx >> hm_detail::kLocalBits) & (hm_detail::kSegsPerChunk - 1),
            bidx & (hm_detail::kBucketsPerSeg - 1)
        };
    }

    // ── CoW helpers ───────────────────────────────────────────────────

    // Build a new block chain from a flat entry array.
    uint64_t build_chain(const Entry* ents, std::size_t n) {
        if (!n) return 0;
        uint64_t head = 0, prev = 0;
        for (std::size_t i = 0; i < n; ) {
            uint32_t cap = static_cast<uint32_t>(
                std::min<std::size_t>(n - i, hm_detail::kBlockCap));
            void* mem  = alloc_->Malloc(blk_alloc_sz());
            auto* blk  = static_cast<Block*>(mem);
            blk->count       = cap;
            blk->capacity    = hm_detail::kBlockCap;
            blk->entry_bytes = static_cast<uint32_t>(sizeof(Entry));
            blk->next        = 0;
            std::memcpy(blk_entries(blk), ents + i, cap * sizeof(Entry));
            uint64_t off = off_of(blk);
            if (prev) at<Block>(prev)->next = off;
            if (!head) head = off;
            prev = off;
            i += cap;
        }
        return head;
    }

    // CoW bucket put: collect entries, update or insert, retire old chain.
    uint64_t cow_bucket_put(uint64_t old_head, uint64_t h,
                             const K& k, const V& v, bool& inserted) {
        std::vector<Entry> ents;
        bool found = false;
        for (uint64_t cur = old_head; cur; ) {
            const auto* blk = at<Block>(cur);
            const auto* es  = blk_entries(blk);
            for (uint16_t i = 0; i < blk->count; ++i) {
                if (!found && entry_matches(es[i], k, h)) {
                    found = true;
                    retire_entry_blobs(es[i]);  // retire old blobs
                    Entry updated;
                    make_entry(updated, h, k, v);
                    ents.push_back(updated);
                } else {
                    ents.push_back(es[i]);
                }
            }
            uint64_t nxt = blk->next;
            retire_.push_back(cur);
            cur = nxt;
        }
        if (!found) {
            Entry ne; make_entry(ne, h, k, v); ents.push_back(ne);
        }
        inserted = !found;
        return build_chain(ents.data(), ents.size());
    }

    // CoW bucket erase: collect surviving entries, retire erased blobs.
    uint64_t cow_bucket_erase(uint64_t old_head, uint64_t h,
                               const K& k, bool& erased) {
        std::vector<Entry> ents;
        erased = false;
        for (uint64_t cur = old_head; cur; ) {
            const auto* blk = at<Block>(cur);
            const auto* es  = blk_entries(blk);
            for (uint16_t i = 0; i < blk->count; ++i) {
                if (!erased && entry_matches(es[i], k, h)) {
                    erased = true;
                    retire_entry_blobs(es[i]);
                } else {
                    ents.push_back(es[i]);
                }
            }
            uint64_t nxt = blk->next;
            retire_.push_back(cur);
            cur = nxt;
        }
        return build_chain(ents.data(), ents.size());
    }

    // CoW segment: copy, update one bucket slot.
    uint64_t cow_segment(uint64_t old_off, uint64_t local, uint64_t new_bkt) {
        void* mem  = alloc_->Malloc(sizeof(Segment));
        auto* seg  = static_cast<Segment*>(mem);
        if (old_off) {
            std::memcpy(seg, at<Segment>(old_off), sizeof(Segment));
            retire_.push_back(old_off);
        } else {
            std::memset(seg, 0, sizeof(Segment));
        }
        seg->bkt_off[local] = new_bkt;
        return off_of(seg);
    }

    // CoW dir-chunk: copy, update one segment slot.
    uint64_t cow_chunk(uint64_t old_off, uint64_t seg_idx, uint64_t new_seg) {
        void*  mem   = alloc_->Malloc(sizeof(DirChunk));
        auto*  chunk = static_cast<DirChunk*>(mem);
        if (old_off) {
            std::memcpy(chunk, at<DirChunk>(old_off), sizeof(DirChunk));
            retire_.push_back(old_off);
        } else {
            std::memset(chunk, 0, sizeof(DirChunk));
        }
        chunk->seg_off[seg_idx] = new_seg;
        return off_of(chunk);
    }

    // CoW root: copy, update one chunk slot and entry count.
    uint64_t cow_root(uint64_t old_off, uint64_t chunk_idx,
                      uint64_t new_chunk, int64_t delta) {
        void* mem  = alloc_->Malloc(sizeof(Root));
        auto* r    = static_cast<Root*>(mem);
        std::memcpy(r, at<Root>(old_off), sizeof(Root));
        r->chunk_off[chunk_idx] = new_chunk;
        r->entry_count = static_cast<uint64_t>(
            static_cast<int64_t>(r->entry_count) + delta);
        if (chunk_idx >= r->chunk_count)
            r->chunk_count = static_cast<uint32_t>(chunk_idx + 1);
        retire_.push_back(old_off);
        return off_of(r);
    }

    // Low-level put into a given root (used by rehash).
    void do_put(uint64_t h, const K& k, const V& v, bool& inserted) {
        const auto* root = at<Root>(staged_root_);
        auto idx = decompose(h, root);

        uint64_t old_chunk = root->chunk_off[idx.chunk];
        uint64_t old_seg   = old_chunk
                                 ? at<DirChunk>(old_chunk)->seg_off[idx.seg] : 0;
        uint64_t old_bkt   = old_seg
                                 ? at<Segment>(old_seg)->bkt_off[idx.local] : 0;

        uint64_t new_bkt   = cow_bucket_put(old_bkt, h, k, v, inserted);
        uint64_t new_seg   = cow_segment(old_chunk ? old_seg : 0, idx.local, new_bkt);
        uint64_t new_chunk = cow_chunk(old_chunk, idx.seg, new_seg);
        staged_root_ = cow_root(staged_root_, idx.chunk, new_chunk,
                                 inserted ? 1 : 0);
    }

    // Iterate every live entry under a root (read-only, used by rehash).
    template <class Fn>
    void for_each_in_root(const Root* root, Fn&& fn) const {
        for (uint32_t ci = 0; ci < root->chunk_count; ++ci) {
            if (!root->chunk_off[ci]) continue;
            const auto* chunk = at<DirChunk>(root->chunk_off[ci]);
            for (uint32_t si = 0; si < hm_detail::kSegsPerChunk; ++si) {
                if (!chunk->seg_off[si]) continue;
                const auto* seg = at<Segment>(chunk->seg_off[si]);
                for (uint32_t bi = 0; bi < hm_detail::kBucketsPerSeg; ++bi) {
                    for (uint64_t cur = seg->bkt_off[bi]; cur; ) {
                        const auto* blk = at<Block>(cur);
                        const auto* es  = blk_entries(blk);
                        for (uint16_t i = 0; i < blk->count; ++i) fn(es[i]);
                        cur = blk->next;
                    }
                }
            }
        }
    }

    // Mark every node in the tree rooted at root_off for delayed retirement.
    void retire_root_tree(uint64_t root_off) {
        if (!root_off) return;
        const auto* root = at<Root>(root_off);
        for (uint32_t ci = 0; ci < root->chunk_count; ++ci) {
            if (!root->chunk_off[ci]) continue;
            const auto* chunk = at<DirChunk>(root->chunk_off[ci]);
            for (uint32_t si = 0; si < hm_detail::kSegsPerChunk; ++si) {
                if (!chunk->seg_off[si]) continue;
                const auto* seg = at<Segment>(chunk->seg_off[si]);
                for (uint32_t bi = 0; bi < hm_detail::kBucketsPerSeg; ++bi) {
                    for (uint64_t cur = seg->bkt_off[bi]; cur; ) {
                        const auto* blk = at<Block>(cur);
                        const auto* es  = blk_entries(blk);
                        for (uint16_t i = 0; i < blk->count; ++i)
                            retire_entry_blobs(es[i]);
                        uint64_t nxt = blk->next;
                        retire_.push_back(cur);
                        cur = nxt;
                    }
                }
                retire_.push_back(chunk->seg_off[si]);
            }
            retire_.push_back(root->chunk_off[ci]);
        }
        retire_.push_back(root_off);
    }

    // Double the bucket count and rebuild the directory.
    void rehash() {
        const auto* old_root    = at<Root>(staged_root_);
        uint32_t new_bits       = old_root->bucket_bits + 1;
        uint64_t new_bkt_count  = uint64_t{1} << new_bits;
        uint64_t new_chunk_n    = new_bkt_count >>
                                  (hm_detail::kLocalBits + hm_detail::kSegBits);
        if (!new_chunk_n) new_chunk_n = 1;
        if (new_chunk_n > hm_detail::kMaxDirChunks)
            throw std::overflow_error("HashMap: max bucket count exceeded");

        // Fresh root for the new layout.
        void* mem      = alloc_->Malloc(sizeof(Root));
        auto* new_root = static_cast<Root*>(mem);
        std::memset(new_root, 0, sizeof(Root));
        new_root->bucket_count = new_bkt_count;
        new_root->bucket_bits  = new_bits;

        uint64_t old_staged = staged_root_;
        staged_root_        = off_of(new_root);

        // Re-insert all entries into the new layout.
        for_each_in_root(old_root, [&](const Entry& e) {
            bool ins = false;
            do_put(entry_hash(e), entry_key(e), entry_value(e), ins);
        });

        retire_root_tree(old_staged);
    }

    static uint64_t hm_steady_ns() noexcept {
        return static_cast<uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());
    }

    // ── Members ───────────────────────────────────────────────────────
    Allocator*                alloc_       = nullptr;
    void*                     base_        = nullptr;
    std::atomic<uint64_t>     pub_root_{0};   // published root (readers)
    uint64_t                  staged_root_ = 0; // writer-side current root
    uint64_t                  hdr_off_     = 0; // arena offset of HmHeader (stable)

    // Process-local ephemeral state (not persisted).
    // Nodes accumulated here during puts/erases are moved to retired_batches_
    // at publish() and freed once their age exceeds reclaim_delay_ns_.
    std::vector<uint64_t>     retire_;   // pending since last publish

    struct RetiredBatch {
        uint64_t              free_time_ns;  // steady_clock ns when publish() ran
        std::vector<uint64_t> offsets;
    };
    std::vector<RetiredBatch> retired_batches_;

public:
    // ============================================================
    // Snapshot — lock-free reader view
    // ============================================================
    class Snapshot {
        const void* base_;
        const Root* root_;

        uint64_t first_block(uint64_t h) const noexcept {
            if (!root_) return 0;
            uint64_t bidx = h & (root_->bucket_count - 1);
            uint64_t ci   = bidx >> (hm_detail::kLocalBits + hm_detail::kSegBits);
            uint64_t si   = (bidx >> hm_detail::kLocalBits) &
                            (hm_detail::kSegsPerChunk - 1);
            uint64_t li   = bidx & (hm_detail::kBucketsPerSeg - 1);
            uint64_t co   = root_->chunk_off[ci];   if (!co) return 0;
            uint64_t so   = hm_detail::at<DirChunk>(base_, co)->seg_off[si];
            if (!so) return 0;
            return hm_detail::at<Segment>(base_, so)->bkt_off[li];
        }

        bool matches(const Entry& e, const K& k, uint64_t h) const {
            if constexpr (kInline) {
                const auto& ie = static_cast<const hm_detail::HmInlineEntry<K, V>&>(e);
                return ie.hash == h && Eq{}(ie.key, k);
            } else {
                const auto& be = static_cast<const hm_detail::HmBlobEntry&>(e);
                return be.hash == h &&
                       Eq{}(KCodec::decode(base_, be.key_off, be.key_len), k);
            }
        }

        V val_of(const Entry& e) const {
            if constexpr (kInline)
                return static_cast<const hm_detail::HmInlineEntry<K, V>&>(e).val;
            else {
                const auto& be = static_cast<const hm_detail::HmBlobEntry&>(e);
                return VCodec::decode(base_, be.val_off, be.val_len);
            }
        }

    public:
        Snapshot() noexcept : base_(nullptr), root_(nullptr) {}
        Snapshot(const void* base, const Root* root) noexcept
            : base_(base), root_(root) {}

        bool   empty() const noexcept { return !root_ || root_->entry_count == 0; }
        size_t size()  const noexcept { return root_ ? root_->entry_count : 0; }

        bool get(const K& k, V& out) const {
            if (!root_) return false;
            uint64_t h = Hash{}(k);
            for (uint64_t cur = first_block(h); cur; ) {
                const auto* blk = hm_detail::at<Block>(base_, cur);
                const auto* es  = reinterpret_cast<const Entry*>(
                    reinterpret_cast<const char*>(blk) + sizeof(Block));
                for (uint16_t i = 0; i < blk->count; ++i) {
                    if (matches(es[i], k, h)) { out = val_of(es[i]); return true; }
                }
                cur = blk->next;
            }
            return false;
        }

        bool contains(const K& k) const { V v; return get(k, v); }

        template <class Fn>
        void for_each(Fn&& fn) const {
            if (!root_) return;
            for (uint32_t ci = 0; ci < root_->chunk_count; ++ci) {
                if (!root_->chunk_off[ci]) continue;
                const auto* chunk = hm_detail::at<DirChunk>(base_, root_->chunk_off[ci]);
                for (uint32_t si = 0; si < hm_detail::kSegsPerChunk; ++si) {
                    if (!chunk->seg_off[si]) continue;
                    const auto* seg = hm_detail::at<Segment>(base_, chunk->seg_off[si]);
                    for (uint32_t bi = 0; bi < hm_detail::kBucketsPerSeg; ++bi) {
                        for (uint64_t cur = seg->bkt_off[bi]; cur; ) {
                            const auto* blk = hm_detail::at<Block>(base_, cur);
                            const auto* es  = reinterpret_cast<const Entry*>(
                                reinterpret_cast<const char*>(blk) + sizeof(Block));
                            for (uint16_t i = 0; i < blk->count; ++i) {
                                if constexpr (kInline) {
                                    const auto& ie =
                                        static_cast<const hm_detail::HmInlineEntry<K, V>&>(es[i]);
                                    fn(ie.key, ie.val);
                                } else {
                                    const auto& be =
                                        static_cast<const hm_detail::HmBlobEntry&>(es[i]);
                                    fn(KCodec::decode(base_, be.key_off, be.key_len),
                                       VCodec::decode(base_, be.val_off, be.val_len));
                                }
                            }
                            cur = blk->next;
                        }
                    }
                }
            }
        }
    };

    // ============================================================
    // Construction / recovery
    // ============================================================

    // hdr_off == 0 → allocate a new HmHeader + HmRoot (first use).
    // hdr_off != 0 → recover from existing HmHeader (after mmap reopen).
    // The hdr_off returned by root_offset() is stable across all puts.
    explicit HashMap(Allocator* alloc, uint64_t hdr_off = 0,
                     uint32_t bucket_bits = 12)
        : alloc_(alloc), base_(alloc->BaseAddress()) {
        if (hdr_off != 0) {
            // Recovery: validate header and restore current root.
            const auto* hdr = at<hm_detail::HmHeader>(hdr_off);
            if (hdr->magic   != hm_detail::kMagic)
                throw std::runtime_error("HashMap: bad magic on recovery");
            if (hdr->version != hm_detail::kVersion)
                throw std::runtime_error("HashMap: unsupported version");
            hdr_off_     = hdr_off;
            staged_root_ = hdr->root_off;
        } else {
            uint64_t bucket_count = uint64_t{1} << bucket_bits;
            uint64_t chunk_count  = bucket_count >>
                                    (hm_detail::kLocalBits + hm_detail::kSegBits);
            if (!chunk_count) chunk_count = 1;
            if (chunk_count > hm_detail::kMaxDirChunks)
                throw std::invalid_argument("HashMap: too many buckets requested");

            // Allocate the initial (empty) HmRoot.
            void* rmem = alloc_->Malloc(sizeof(Root));
            auto* r    = static_cast<Root*>(rmem);
            std::memset(r, 0, sizeof(Root));
            r->entry_count  = 0;
            r->bucket_count = bucket_count;
            r->bucket_bits  = bucket_bits;
            r->chunk_count  = 0;
            staged_root_ = off_of(r);

            // Allocate the stable HmHeader pointing to this root.
            void* hmem = alloc_->Malloc(sizeof(hm_detail::HmHeader));
            auto* hdr  = static_cast<hm_detail::HmHeader*>(hmem);
            hdr->magic   = hm_detail::kMagic;
            hdr->version = hm_detail::kVersion;
            hdr->_pad    = 0;
            hdr->root_off = staged_root_;
            hdr_off_ = off_of(hdr);
        }
        alloc_->PublishFence();
        pub_root_.store(staged_root_, std::memory_order_release);
    }

    HashMap(const HashMap&)            = delete;
    HashMap& operator=(const HashMap&) = delete;
    HashMap(HashMap&&) noexcept        = default;
    HashMap& operator=(HashMap&&) noexcept = default;

    ~HashMap() = default;

    // Arena offset of the stable HmHeader.  Store this in the parent
    // structure once; it never changes even as the map grows via CoW.
    uint64_t root_offset() const noexcept { return hdr_off_; }

    // ============================================================
    // Writer API  (single thread only)
    // ============================================================

    // Staged (writer-side) lookup — reads from staged_root_, not the published
    // root.  Call this BEFORE put/erase to retrieve the current value so that
    // the caller can retire associated resources before they are overwritten.
    bool staged_get(const K& k, V& out) const {
        const auto* root = at<Root>(staged_root_);
        if (!root || !root->entry_count) return false;
        uint64_t h   = Hash{}(k);
        auto     idx = decompose(h, root);
        uint64_t old_chunk = root->chunk_off[idx.chunk];
        if (!old_chunk) return false;
        uint64_t old_seg = at<DirChunk>(old_chunk)->seg_off[idx.seg];
        if (!old_seg)   return false;
        uint64_t old_bkt = at<Segment>(old_seg)->bkt_off[idx.local];
        for (uint64_t cur = old_bkt; cur; ) {
            const auto* blk = at<Block>(cur);
            const auto* es  = blk_entries(blk);
            for (uint16_t i = 0; i < blk->count; ++i) {
                if (entry_matches(es[i], k, h)) {
                    out = entry_value(es[i]);
                    return true;
                }
            }
            cur = blk->next;
        }
        return false;
    }

    // Insert or update key → value.  Does NOT publish.
    void put(const K& k, const V& v) {
        const auto* root = at<Root>(staged_root_);
        if (root->entry_count >=
            root->bucket_count * hm_detail::kLoadFactor) {
            rehash();
        }
        bool inserted = false;
        do_put(Hash{}(k), k, v, inserted);
    }

    // Remove key.  Returns true if the key was present.  Does NOT publish.
    bool erase(const K& k) {
        uint64_t h    = Hash{}(k);
        const auto* root = at<Root>(staged_root_);
        auto idx      = decompose(h, root);

        uint64_t old_chunk = root->chunk_off[idx.chunk];
        if (!old_chunk) return false;
        uint64_t old_seg = at<DirChunk>(old_chunk)->seg_off[idx.seg];
        if (!old_seg)   return false;
        uint64_t old_bkt = at<Segment>(old_seg)->bkt_off[idx.local];
        if (!old_bkt)   return false;

        bool erased = false;
        uint64_t new_bkt   = cow_bucket_erase(old_bkt, h, k, erased);
        if (!erased) return false;

        uint64_t new_seg   = cow_segment(old_seg, idx.local, new_bkt);
        uint64_t new_chunk = cow_chunk(old_chunk, idx.seg, new_seg);
        staged_root_ = cow_root(staged_root_, idx.chunk, new_chunk, -1);
        return true;
    }

    // Publish all pending puts/erases atomically.
    // After this call, acquire_snapshot() sees the new state.
    // Retired CoW nodes are swept inline: any batch older than
    // alloc_->ReclaimDelayNs() is freed immediately.
    void publish() {
        at<hm_detail::HmHeader>(hdr_off_)->root_off = staged_root_;
        alloc_->PublishFence();
        pub_root_.store(staged_root_, std::memory_order_release);
        alloc_->AdvanceEpoch();  // kept for sequencing / stats
        if (!retire_.empty()) {
            retired_batches_.push_back({hm_steady_ns(), std::move(retire_)});
            retire_.clear();
        }
        // Sweep expired batches inline — no external reclaim() call needed.
        const uint64_t delay  = alloc_->ReclaimDelayNs();
        const uint64_t now    = hm_steady_ns();
        const uint64_t cutoff = (now > delay) ? now - delay : 0;
        while (!retired_batches_.empty() &&
               retired_batches_.front().free_time_ns <= cutoff) {
            for (uint64_t off : retired_batches_.front().offsets)
                alloc_->Free(static_cast<char*>(base_) + off,
                             FreeMode::Immediate);
            retired_batches_.erase(retired_batches_.begin());
        }
        alloc_->ReclaimExpired();
    }

    size_t size() const noexcept { return at<Root>(staged_root_)->entry_count; }

    // ============================================================
    // Reader API  (any thread, lock-free)
    // ============================================================

    Snapshot acquire_snapshot() const noexcept {
        uint64_t off = pub_root_.load(std::memory_order_acquire);
        if (!off) return {};
        return Snapshot(base_, at<Root>(off));
    }
};

}  // namespace container
}  // namespace yikv
