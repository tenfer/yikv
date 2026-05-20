#pragma once

// ConcurrentHashMap<K,V> — arena-backed, multi-reader multi-writer hash map for
// use with AllocatorMode::Concurrent (see alloc/allocator.h).
//
// Persistence: MxMapHeader + flat bucket head table (8-byte slots) + HmBlock
// chains compatible with HashMap entry layouts (inline or blob).
//
// Concurrency (v2 — optimized):
//   1. Seqlock (table_seq_, even=stable, odd=rehash): replaces table_mtx_
//      shared_mutex on every hot-path operation.  Only rehash increments seq.
//   2. Per-stripe std::mutex (replaces std::shared_mutex): lower overhead for
//      mixed / write-heavy workloads; concurrent readers should use
//      enable_lock_free_reads() instead.
//   3. lock_free_reads_ flag checked with relaxed memory order in put/erase
//      (the subsequent mutex acquire already provides the necessary fence).
//   4. Per-stripe cache-line-aligned entry counts (stripe_counts_) avoid
//      false-sharing on the mmap preamble entry_count; preamble is only
//      synced at rehash time.
//   5. needs_rehash() is called at most once every 64 puts per thread via a
//      thread_local counter, amortising the cost of summing stripe_counts_.
//
// Lock-free get: after enable_lock_free_reads() with all writers quiesced,
//   get() skips seqlock and stripe mutexes entirely.
//
// Deallocation: erased blocks and replaced blob payloads use FreeMode::Delayed
//   so readers still traversing an old chain tail do not see use-after-free;
//   see concurrent_hashmap.md.  Requires reclaim_delay_ns > max read latency.

#include "alloc/allocator.h"
#include "container/hashmap.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace yikv {
namespace container {

namespace mx_detail {

constexpr uint32_t kMxMagic   = 0x434d5848u;  // "HMXC" little-endian
constexpr uint16_t kMxVersion = 1;

// Preamble before bucket head table: stable, identifies map on mmap recover.
struct alignas(8) MxHeadTablePreamble {
    uint32_t magic;           // kMxMagic
    uint16_t version;         // kMxVersion
    uint8_t  stripe_shift;    // num_stripes = 1u << stripe_shift
    uint8_t  bucket_bits;     // bucket_count = 1u << bucket_bits
    uint32_t reserved;
    uint64_t entry_count;     // persisted size; synced at rehash
};
static_assert(sizeof(MxHeadTablePreamble) == 24);

inline constexpr uint32_t kMaxStripeShift = 16;

inline uint64_t atomic_load_u64(const uint64_t* p) noexcept {
    return __atomic_load_n(const_cast<uint64_t*>(p), __ATOMIC_ACQUIRE);
}
inline void atomic_store_u64(uint64_t* p, uint64_t v) noexcept {
    __atomic_store_n(p, v, __ATOMIC_RELEASE);
}
inline uint64_t atomic_load_u64_relaxed(const uint64_t* p) noexcept {
    return __atomic_load_n(const_cast<uint64_t*>(p), __ATOMIC_RELAXED);
}
inline void atomic_store_u64_relaxed(uint64_t* p, uint64_t v) noexcept {
    __atomic_store_n(p, v, __ATOMIC_RELAXED);
}
inline uint64_t atomic_fetch_add_u64_relaxed(uint64_t* p, uint64_t delta) noexcept {
    return __atomic_fetch_add(p, delta, __ATOMIC_RELAXED);
}
inline uint64_t atomic_fetch_sub_u64_relaxed(uint64_t* p, uint64_t delta) noexcept {
    return __atomic_fetch_sub(p, delta, __ATOMIC_RELAXED);
}

inline void* at_bytes(void* base, uint64_t off) noexcept {
    return static_cast<char*>(base) + off;
}
inline const void* at_bytes(const void* base, uint64_t off) noexcept {
    return static_cast<const char*>(base) + off;
}

}  // namespace mx_detail

// Reuse HashMap codecs / entry layout from hashmap.h (hm_detail + DefaultCodec).

template <
    class K,
    class V,
    class Hash   = std::hash<K>,
    class Eq     = std::equal_to<K>,
    class KCodec = DefaultCodec<K>,
    class VCodec = DefaultCodec<V>>
class ConcurrentHashMap {
    using Entry    = hm_detail::HmEntry<K, V>;
    using Block    = hm_detail::HmBlock;
    static constexpr bool kInline =
        std::is_trivially_copyable_v<K> && std::is_trivially_copyable_v<V>;

    alloc::Allocator*       alloc_ = nullptr;
    void*                   base_  = nullptr;
    uint64_t                head_region_off_ = 0;
    uint64_t                heads_base_off_  = 0;
    uint32_t                bucket_bits_     = 0;
    uint32_t                bucket_count_    = 0;
    uint32_t                num_stripes_     = 0;
    uint32_t                stripe_mask_     = 0;

    // Opt 1: seqlock — even = table stable, odd = rehash in progress.
    mutable std::atomic<uint32_t> table_seq_{0};
    mutable std::mutex            rehash_mtx_;

    // Opt 2: std::mutex per stripe (lower uncontended overhead than shared_mutex).
    mutable std::vector<std::unique_ptr<std::mutex>> stripes_;

    // Opt 4: per-stripe entry counts, cache-line-aligned to avoid false sharing.
    // Preamble entry_count is only synced at rehash time.
    struct alignas(64) StripeCount { std::atomic<int64_t> n{0}; };
    std::vector<StripeCount> stripe_counts_;

    mutable std::atomic<bool> lock_free_reads_{false};

    // ── address helpers ──────────────────────────────────────────────────────

    template <class T> T* at(uint64_t off) noexcept {
        return hm_detail::at<T>(base_, off);
    }
    template <class T> const T* at(uint64_t off) const noexcept {
        return hm_detail::at<T>(base_, off);
    }
    uint64_t off_of(const void* p) const noexcept {
        return hm_detail::off_of(base_, p);
    }

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

    static uint64_t entry_hash(const Entry& e) noexcept {
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

    void make_entry(Entry& e, uint64_t h, const K& k, const V& v) {
        if constexpr (kInline) {
            auto& ie = static_cast<hm_detail::HmInlineEntry<K, V>&>(e);
            ie.hash = h;
            ie.key  = k;
            ie.val  = v;
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
            if (be.key_off) KCodec::retire(*alloc_, be.key_off);
            if (be.val_off && be.val_len > 0) VCodec::retire(*alloc_, be.val_off);
        }
    }

    // Copy an entry into dst without tearing.
    // For inline entries (trivially copyable, <= 8-16 bytes), a single write
    // is atomic on all supported platforms.  For blob entries we write the
    // header fields (hash) first, then the offsets/lengths, so a lock-free
    // reader either sees the old complete entry or the new one — never a mix
    // of old offsets with a new hash (which would cause a false match).
    void copy_entry(const Entry& src, Entry& dst) {
        if constexpr (kInline) {
            // Single-word or double-word copy — atomic on aligned stores.
            dst = src;
        } else {
            const auto& bs = static_cast<const hm_detail::HmBlobEntry&>(src);
            auto& bd = static_cast<hm_detail::HmBlobEntry&>(dst);
            // Write new offsets/lengths first, then hash last.
            // Readers check hash before accessing offsets, so they either
            // see the old entry (old hash) or skip a mismatching new one.
            bd.key_off = bs.key_off;
            bd.key_len = bs.key_len;
            bd.val_off = bs.val_off;
            bd.val_len = bs.val_len;
            // hash is the guard — written last so reader sees consistent state.
            bd.hash = bs.hash;
        }
    }

    void retire_block_tree(uint64_t head_off) {
        for (uint64_t cur = head_off; cur; ) {
            Block* blk = at<Block>(cur);
            if constexpr (!kInline) {
                const Entry* es = blk_entries(blk);
                for (uint16_t i = 0; i < blk->count; ++i) retire_entry_blobs(es[i]);
            }
            uint64_t nxt = blk->next;
            alloc_->Free(static_cast<char*>(base_) + cur, alloc::FreeMode::Delayed);
            cur = nxt;
        }
    }

    mx_detail::MxHeadTablePreamble* preamble() noexcept {
        return reinterpret_cast<mx_detail::MxHeadTablePreamble*>(
            mx_detail::at_bytes(base_, head_region_off_));
    }
    const mx_detail::MxHeadTablePreamble* preamble() const noexcept {
        return reinterpret_cast<const mx_detail::MxHeadTablePreamble*>(
            mx_detail::at_bytes(base_, head_region_off_));
    }

    uint64_t* head_slot(uint32_t bucket_idx) noexcept {
        return reinterpret_cast<uint64_t*>(mx_detail::at_bytes(base_, heads_base_off_)) +
               bucket_idx;
    }
    const uint64_t* head_slot(uint32_t bucket_idx) const noexcept {
        return reinterpret_cast<const uint64_t*>(mx_detail::at_bytes(base_, heads_base_off_)) +
               bucket_idx;
    }

    uint64_t load_head(uint32_t bidx) const noexcept {
        return mx_detail::atomic_load_u64(head_slot(bidx));
    }
    void store_head(uint32_t bidx, uint64_t v) noexcept {
        mx_detail::atomic_store_u64(head_slot(bidx), v);
    }

    uint64_t* entry_count_ptr() noexcept {
        return &preamble()->entry_count;
    }

    uint32_t stripe_index(uint32_t bucket_idx) const noexcept {
        return bucket_idx & stripe_mask_;
    }

    void validate_preamble(const mx_detail::MxHeadTablePreamble* p) const {
        if (p->magic != mx_detail::kMxMagic)
            throw std::runtime_error("ConcurrentHashMap: bad magic");
        if (p->version != mx_detail::kMxVersion)
            throw std::runtime_error("ConcurrentHashMap: unsupported version");
        if (p->stripe_shift > mx_detail::kMaxStripeShift)
            throw std::runtime_error("ConcurrentHashMap: stripe_shift too large");
        if (p->bucket_bits == 0 || p->bucket_bits > 62)
            throw std::runtime_error("ConcurrentHashMap: invalid bucket_bits");
    }

    void lock_all_stripes_exclusive(
            std::vector<std::unique_lock<std::mutex>>& out) {
        out.clear();
        out.reserve(stripes_.size());
        for (auto& m : stripes_) out.emplace_back(*m);
    }

    // Opt 4: sum all stripe counts (called only ~every 64 puts, via trigger_rehash).
    bool needs_rehash() const noexcept {
        int64_t total = 0;
        for (const auto& sc : stripe_counts_)
            total += sc.n.load(std::memory_order_relaxed);
        const uint64_t cap =
            static_cast<uint64_t>(bucket_count_) * hm_detail::kLoadFactor;
        return total >= static_cast<int64_t>(cap);
    }

    // ── rehash ───────────────────────────────────────────────────────────────

    // Called while holding all stripe locks (rehash_mtx_ also held by caller).
    void rehash_locked() {
        const uint32_t new_bits  = bucket_bits_ + 1;
        const uint32_t old_count = bucket_count_;
        const uint32_t new_count = 1u << new_bits;
        if (new_bits > 62) throw std::overflow_error("ConcurrentHashMap: bucket_bits");

        // Preamble entry_count is kept accurate by per-put/erase writes (see put/erase).
        // Use it as the canonical snapshot; reset per-stripe counters (all stripes locked).
        const uint64_t old_entry_snapshot =
            mx_detail::atomic_load_u64_relaxed(entry_count_ptr());
        for (auto& sc : stripe_counts_)
            sc.n.store(0, std::memory_order_relaxed);

        const std::size_t preamble_sz = sizeof(mx_detail::MxHeadTablePreamble);
        const std::size_t new_heads_sz =
            static_cast<std::size_t>(new_count) * sizeof(uint64_t);
        void* region = alloc_->Malloc(preamble_sz + new_heads_sz);
        std::memset(region, 0, preamble_sz + new_heads_sz);

        auto* pre = static_cast<mx_detail::MxHeadTablePreamble*>(region);
        pre->magic         = mx_detail::kMxMagic;
        pre->version       = mx_detail::kMxVersion;
        pre->stripe_shift  = static_cast<uint8_t>(
            num_stripes_ > 0 ? __builtin_ctz(num_stripes_) : 0);
        pre->bucket_bits   = static_cast<uint8_t>(new_bits);
        pre->reserved      = 0;

        const uint64_t new_region_off = off_of(region);
        const uint64_t new_heads_off  = new_region_off + preamble_sz;

        // Collect all entries and free old blocks.
        std::vector<Entry> flat;
        flat.reserve(static_cast<std::size_t>(old_entry_snapshot ? old_entry_snapshot : 16));
        for (uint32_t b = 0; b < old_count; ++b) {
            uint64_t cur = load_head(b);
            while (cur) {
                Block* blk = at<Block>(cur);
                Entry* es  = blk_entries(blk);
                for (uint16_t i = 0; i < blk->count; ++i)
                    flat.push_back(es[i]);
                uint64_t nxt = blk->next;
                alloc_->Free(static_cast<char*>(base_) + cur, alloc::FreeMode::Delayed);
                cur = nxt;
            }
        }
        alloc_->Free(mx_detail::at_bytes(base_, head_region_off_),
                     alloc::FreeMode::Delayed);

        head_region_off_ = new_region_off;
        heads_base_off_  = new_heads_off;
        bucket_bits_     = new_bits;
        bucket_count_    = new_count;

        // Re-insert into new table (single-threaded; all stripes are locked).
        for (const Entry& e : flat) {
            const uint64_t h        = entry_hash(e);
            const uint32_t bidx     = static_cast<uint32_t>(h & (new_count - 1));
            uint64_t head =
                mx_detail::atomic_load_u64_relaxed(head_slot(bidx));
            bool need_new_block = true;
            if (head) {
                uint64_t last = 0, p = head;
                while (p) {
                    Block* blk = at<Block>(p);
                    if (blk->count < hm_detail::kBlockCap) {
                        blk_entries(blk)[blk->count] = e;
                        ++blk->count;
                        need_new_block = false;
                        break;
                    }
                    last = p;
                    p    = blk->next;
                }
                if (need_new_block) {
                    void* mem      = alloc_->Malloc(blk_alloc_sz());
                    auto* blk      = static_cast<Block*>(mem);
                    blk->count       = 1;
                    blk->capacity    = hm_detail::kBlockCap;
                    blk->entry_bytes = static_cast<uint32_t>(sizeof(Entry));
                    blk->next        = 0;
                    blk_entries(blk)[0] = e;
                    at<Block>(last)->next = off_of(blk);
                }
            } else {
                void* mem = alloc_->Malloc(blk_alloc_sz());
                auto* blk = static_cast<Block*>(mem);
                blk->count       = 1;
                blk->capacity    = hm_detail::kBlockCap;
                blk->entry_bytes = static_cast<uint32_t>(sizeof(Entry));
                blk->next        = 0;
                blk_entries(blk)[0] = e;
                mx_detail::atomic_store_u64_relaxed(head_slot(bidx), off_of(blk));
            }
        }

        // Sync persisted entry_count to preamble.
        mx_detail::atomic_store_u64_relaxed(entry_count_ptr(), old_entry_snapshot);
        // Restore stripe_counts_[0] so needs_rehash() is correct after rehash.
        if (old_entry_snapshot > 0)
            stripe_counts_[0].n.store(static_cast<int64_t>(old_entry_snapshot),
                                      std::memory_order_relaxed);
    }

    // Opt 1: stop-the-world rehash via seqlock + rehash_mtx_.
    // Called from put() after releasing its stripe lock.
    void trigger_rehash() {
        std::unique_lock<std::mutex> guard(rehash_mtx_);
        if (!needs_rehash()) return;  // another thread already did it

        // Signal readers/writers: table is unstable (odd seq).
        table_seq_.fetch_add(1u, std::memory_order_seq_cst);

        // Acquire all stripes — waits for any in-flight put/get/erase to finish.
        std::vector<std::unique_lock<std::mutex>> stripe_guards;
        lock_all_stripes_exclusive(stripe_guards);

        if (needs_rehash())
            rehash_locked();

        // Mark table stable again BEFORE releasing stripes so that threads
        // spinning on the seqlock compute bidx with the new bucket_count_.
        table_seq_.fetch_add(1u, std::memory_order_release);
        // stripe_guards destroyed → stripes released → guard released
    }

public:
    explicit ConcurrentHashMap(alloc::Allocator* alloc,
                               uint64_t          head_region_off = 0,
                               uint32_t          bucket_bits     = 10,
                               uint8_t           stripe_shift    = 6)
        : alloc_(alloc), base_(alloc->BaseAddress()) {
        if (!alloc || !alloc->IsOpen())
            throw std::invalid_argument("ConcurrentHashMap: allocator not open");
        if (stripe_shift > mx_detail::kMaxStripeShift)
            throw std::invalid_argument("ConcurrentHashMap: stripe_shift");

        const uint32_t ns = 1u << stripe_shift;
        num_stripes_  = ns;
        stripe_mask_  = ns - 1;

        stripes_.clear();
        stripes_.reserve(static_cast<std::size_t>(ns));
        for (uint32_t i = 0; i < ns; ++i)
            stripes_.emplace_back(std::make_unique<std::mutex>());

        // Opt 4: allocate stripe_counts_ (default-init to 0).
        stripe_counts_ = std::vector<StripeCount>(static_cast<std::size_t>(ns));

        if (head_region_off != 0) {
            head_region_off_ = head_region_off;
            const auto* pre = preamble();
            validate_preamble(pre);
            bucket_bits_  = pre->bucket_bits;
            bucket_count_ = 1u << bucket_bits_;
            heads_base_off_ =
                head_region_off_ + sizeof(mx_detail::MxHeadTablePreamble);
            const uint32_t recovered_stripes = 1u << pre->stripe_shift;
            if (recovered_stripes != num_stripes_)
                throw std::runtime_error(
                    "ConcurrentHashMap: stripe_shift mismatch on recovery");

            // Seed stripe_counts_[0] from the persisted count so needs_rehash()
            // works correctly right after recovery.
            const int64_t existing = static_cast<int64_t>(
                mx_detail::atomic_load_u64_relaxed(&pre->entry_count));
            if (existing > 0)
                stripe_counts_[0].n.store(existing, std::memory_order_relaxed);
        } else {
            if (bucket_bits == 0 || bucket_bits > 62)
                throw std::invalid_argument("ConcurrentHashMap: bucket_bits");
            bucket_bits_  = bucket_bits;
            bucket_count_ = 1u << bucket_bits;
            const std::size_t preamble_sz = sizeof(mx_detail::MxHeadTablePreamble);
            const std::size_t heads_sz =
                static_cast<std::size_t>(bucket_count_) * sizeof(uint64_t);
            void* region = alloc_->Malloc(preamble_sz + heads_sz);
            std::memset(region, 0, preamble_sz + heads_sz);
            auto* pre    = static_cast<mx_detail::MxHeadTablePreamble*>(region);
            pre->magic         = mx_detail::kMxMagic;
            pre->version       = mx_detail::kMxVersion;
            pre->stripe_shift  = stripe_shift;
            pre->bucket_bits   = static_cast<uint8_t>(bucket_bits_);
            pre->reserved      = 0;
            pre->entry_count   = 0;

            head_region_off_ = off_of(region);
            heads_base_off_  = head_region_off_ + preamble_sz;
        }
    }

    uint64_t head_region_offset() const noexcept { return head_region_off_; }

    // Opt 4: size() sums per-stripe counts (no mmap access).
    size_t size() const noexcept {
        int64_t total = 0;
        for (const auto& sc : stripe_counts_)
            total += sc.n.load(std::memory_order_relaxed);
        return total > 0 ? static_cast<std::size_t>(total) : 0;
    }

    void enable_lock_free_reads() {
        lock_free_reads_.store(true, std::memory_order_release);
    }
    void disable_lock_free_reads() noexcept {
        lock_free_reads_.store(false, std::memory_order_release);
    }
    bool lock_free_reads_enabled() const noexcept {
        return lock_free_reads_.load(std::memory_order_acquire);
    }

    // ── get ──────────────────────────────────────────────────────────────────

    bool get(const K& k, V& out) const {
        const uint64_t h = Hash{}(k);

        // Lock-free fast path (enable_lock_free_reads() must have been called).
        if (lock_free_reads_.load(std::memory_order_acquire)) {
            const uint32_t bc   = bucket_count_;
            const uint32_t bidx = static_cast<uint32_t>(h & (bc - 1));
            for (uint64_t cur = load_head(bidx); cur; ) {
                const Block* blk = at<Block>(cur);
                const Entry* es  = blk_entries(blk);
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

        // Opt 1: seqlock — spin until table is stable, re-verify after stripe lock.
        for (;;) {
            const uint32_t seq = table_seq_.load(std::memory_order_acquire);
            if (seq & 1u) { std::this_thread::yield(); continue; }

            const uint32_t bidx = static_cast<uint32_t>(h & (bucket_count_ - 1));
            const uint32_t si   = stripe_index(bidx);
            // Opt 2: std::mutex (unique_lock; use lock-free path for parallel reads).
            std::unique_lock<std::mutex> lk(*stripes_[si]);
            if (table_seq_.load(std::memory_order_acquire) != seq) continue;

            for (uint64_t cur = load_head(bidx); cur; ) {
                const Block* blk = at<Block>(cur);
                const Entry* es  = blk_entries(blk);
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
    }

    // ── put ──────────────────────────────────────────────────────────────────

    void put(const K& k, const V& v) {
        // Opt 3: relaxed — subsequent mutex acquire provides the necessary fence.
        if (lock_free_reads_.load(std::memory_order_relaxed))
            throw std::logic_error(
                "ConcurrentHashMap: put while lock-free reads enabled");

        const uint64_t h = Hash{}(k);
        // Opt 5: amortise needs_rehash by checking only every 64 puts per thread.
        thread_local uint32_t tl_put_count = 0;

        for (;;) {
            // Opt 1: seqlock — wait for table to be stable.
            const uint32_t seq = table_seq_.load(std::memory_order_acquire);
            if (seq & 1u) { std::this_thread::yield(); continue; }

            const uint32_t bidx = static_cast<uint32_t>(h & (bucket_count_ - 1));
            const uint32_t si   = stripe_index(bidx);
            // Opt 2: std::mutex.
            std::unique_lock<std::mutex> lk(*stripes_[si]);
            if (table_seq_.load(std::memory_order_acquire) != seq) continue;

            // Opt 5: check rehash at most every 64 increments.
            if ((++tl_put_count & 63u) == 0u && needs_rehash()) {
                lk.unlock();
                trigger_rehash();
                continue;
            }

            uint64_t head = load_head(bidx);
            if (head) {
                uint64_t last_off = 0;
                for (uint64_t cur = head; cur; ) {
                    Block* blk = at<Block>(cur);
                    Entry* es  = blk_entries(blk);
                    for (uint16_t i = 0; i < blk->count; ++i) {
                        if (entry_matches(es[i], k, h)) {
                            // Build new entry in a temporary, then copy to es[i].
                            // For blob entries we must save old offsets BEFORE
                            // overwriting, then retire them AFTER the copy —
                            // this ensures lock-free readers never dereference
                            // a retired offset (delayed reclaim provides the
                            // grace period).
                            if constexpr (!kInline) {
                                const auto& be = static_cast<const hm_detail::HmBlobEntry&>(es[i]);
                                uint64_t old_key_off = be.key_off;
                                uint64_t old_key_len = be.key_len;
                                uint64_t old_val_off = be.val_off;
                                uint64_t old_val_len = be.val_len;

                                Entry tmp;
                                make_entry(tmp, h, k, v);
                                copy_entry(tmp, es[i]);

                                if (old_key_off) KCodec::retire(*alloc_, old_key_off);
                                if (old_val_off && old_val_len > 0)
                                    VCodec::retire(*alloc_, old_val_off);
                            } else {
                                Entry tmp;
                                make_entry(tmp, h, k, v);
                                copy_entry(tmp, es[i]);
                            }
                            return;  // update in place — entry count unchanged
                        }
                    }
                    last_off = cur;
                    cur      = blk->next;
                }
                Block* last = at<Block>(last_off);
                if (last->count < hm_detail::kBlockCap) {
                    make_entry(blk_entries(last)[last->count], h, k, v);
                    ++last->count;
                } else {
                    void* mem = alloc_->Malloc(blk_alloc_sz());
                    auto* blk = static_cast<Block*>(mem);
                    blk->count       = 1;
                    blk->capacity    = hm_detail::kBlockCap;
                    blk->entry_bytes = static_cast<uint32_t>(sizeof(Entry));
                    blk->next        = 0;
                    make_entry(blk_entries(blk)[0], h, k, v);
                    last->next = off_of(blk);
                }
            } else {
                void* mem = alloc_->Malloc(blk_alloc_sz());
                auto* blk = static_cast<Block*>(mem);
                blk->count       = 1;
                blk->capacity    = hm_detail::kBlockCap;
                blk->entry_bytes = static_cast<uint32_t>(sizeof(Entry));
                blk->next        = 0;
                make_entry(blk_entries(blk)[0], h, k, v);
                store_head(bidx, off_of(blk));
            }
            // Opt 4: per-stripe count (hot path for needs_rehash / size()).
            stripe_counts_[si].n.fetch_add(1, std::memory_order_relaxed);
            // Keep preamble entry_count accurate for persistence / recovery.
            mx_detail::atomic_fetch_add_u64_relaxed(entry_count_ptr(), 1);
            return;
        }
    }

    // ── erase ────────────────────────────────────────────────────────────────

    bool erase(const K& k) {
        // Opt 3: relaxed load.
        if (lock_free_reads_.load(std::memory_order_relaxed))
            throw std::logic_error(
                "ConcurrentHashMap: erase while lock-free reads enabled");

        const uint64_t h = Hash{}(k);

        for (;;) {
            // Opt 1: seqlock.
            const uint32_t seq = table_seq_.load(std::memory_order_acquire);
            if (seq & 1u) { std::this_thread::yield(); continue; }

            const uint32_t bidx = static_cast<uint32_t>(h & (bucket_count_ - 1));
            const uint32_t si   = stripe_index(bidx);
            // Opt 2: std::mutex.
            std::unique_lock<std::mutex> lk(*stripes_[si]);
            if (table_seq_.load(std::memory_order_acquire) != seq) continue;

            uint64_t head = load_head(bidx);
            if (!head) return false;

            uint64_t cur = head, prev = 0;
            while (cur) {
                Block* blk = at<Block>(cur);
                Entry* es  = blk_entries(blk);
                for (uint16_t i = 0; i < blk->count; ++i) {
                    if (entry_matches(es[i], k, h)) {
                        retire_entry_blobs(es[i]);
                        if (blk->count > 1) {
                            // Shift entries down instead of tail-swap.
                            // Tail-swap (es[i] = es[count-1]) is unsafe for lock-free
                            // readers: a reader who already passed position i would
                            // never see the swapped entry, and a reader at position i
                            // could see a torn multi-word write.
                            // Shift is O(count) but count <= kBlockCap (small constant).
                            std::memmove(&es[i], &es[i + 1],
                                static_cast<size_t>(blk->count - 1 - i) * sizeof(Entry));
                            --blk->count;
                        } else {
                            uint64_t nxt = blk->next;
                            alloc_->Free(static_cast<char*>(base_) + cur,
                                         alloc::FreeMode::Delayed);
                            if (prev)
                                at<Block>(prev)->next = nxt;
                            else
                                store_head(bidx, nxt);
                        }
                        // Opt 4: per-stripe count.
                        stripe_counts_[si].n.fetch_sub(1, std::memory_order_relaxed);
                        // Keep preamble entry_count accurate for persistence.
                        mx_detail::atomic_fetch_sub_u64_relaxed(entry_count_ptr(), 1);
                        return true;
                    }
                }
                prev = cur;
                cur  = blk->next;
            }
            return false;
        }
    }
};

}  // namespace container
}  // namespace yikv
