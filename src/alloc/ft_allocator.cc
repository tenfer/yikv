#include "src/alloc/ft_allocator.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <system_error>
#include <vector>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace yikv { namespace alloc {
namespace {

// ============================================================
// Constants
// ============================================================
constexpr std::uint64_t kMagic        = 0x4654414c4c4f4332ull;  // FTALLOC2
constexpr std::uint32_t kVersion      = 2;
constexpr std::uint16_t kLargeClassId = 0xFFFFu;
constexpr std::uint32_t kBlockMagic   = 0x4642414c;             // FBAL
constexpr std::uint16_t kFlagAllocated = 1u << 0;
constexpr std::uint16_t kFlagDelayed   = 1u << 1;
constexpr std::size_t   kMaxSizeClasses = 64;

// --- Block layout ---
// Every block:
//   [0, 16)  BlockHeader  (magic, class_id, flags, block_size, page_count)
//   [16, …)  Payload      (user data when allocated; FreeChain when free)
//
// This shrinks the header overhead vs v1 (48→16 bytes), which halves the
// minimum allocatable size and greatly improves small-object density.
constexpr std::size_t kHeaderSize   = 16;
constexpr std::size_t kFreeChainSize = 16;  // sizeof(FreeChain)
constexpr std::size_t kMinBlockSize = kHeaderSize + kFreeChainSize;  // 32 B

// Size-class ranges
constexpr std::size_t kTinyStep      = 16;    // step in the tiny range
constexpr std::size_t kMaxTinyTotal  = 320;   // highest tiny block_size (B)
constexpr std::uint32_t kDefaultSlabPages = 4;  // pages per slab fill
constexpr std::size_t kTlcCapacity  = 32;    // thread-local cache depth / class

// ============================================================
// Persistent block structures (stored inside the mmap arena)
// ============================================================

// BlockHeader – 16 bytes, sits at the very start of every block.
struct BlockHeader {
    std::uint32_t magic;       // kBlockMagic — corruption guard
    std::uint16_t class_id;    // size-class index, or kLargeClassId
    std::uint16_t flags;       // kFlagAllocated | kFlagDelayed
    std::uint32_t block_size;  // total block bytes (header + payload)
    std::uint32_t page_count;  // large: pages occupied; small/middle: 0
};
static_assert(sizeof(BlockHeader) == kHeaderSize,
              "BlockHeader must be exactly 16 bytes");

// Returns nanoseconds since an arbitrary fixed point (steady_clock).
inline std::uint64_t steady_ns() noexcept {
    return static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
}

// FreeChain – stored in the PAYLOAD region of a FREE block.
// (Payload and FreeChain share the same location; they're mutually exclusive.)
struct FreeChain {
    std::uint64_t next_offset;   // arena offset of next free block (0 = end)
    std::uint64_t free_time_ns;  // steady_clock nanoseconds when block was deferred
};
static_assert(sizeof(FreeChain) == kFreeChainSize);

inline FreeChain* get_free_chain(BlockHeader* hdr) noexcept {
    return reinterpret_cast<FreeChain*>(
        reinterpret_cast<char*>(hdr) + kHeaderSize);
}

// Per size-class metadata in the arena header.
struct SizeClassMeta {
    std::uint32_t block_size     = 0;
    std::uint32_t pages_per_slab = kDefaultSlabPages;
    std::uint64_t free_head      = 0;   // offset of first free block
    std::uint64_t delayed_head   = 0;   // delayed-free list head
    std::uint64_t delayed_tail   = 0;   // delayed-free list tail
};

// Top-level metadata – placed at offset 0 in the mmap region.
// Persistent stats that are needed on reopen are kept here; ephemeral
// counters (used_bytes, allocation_count …) live in in-memory atomics.
struct AllocatorMetadata {
    std::uint64_t magic            = kMagic;
    std::uint32_t version          = kVersion;
    std::uint32_t page_size        = 4096;
    std::uint64_t arena_size       = 0;
    std::uint64_t data_offset      = 0;   // start of data pages
    std::uint64_t bitmap_offset    = 0;   // start of page bitmap
    std::uint64_t bitmap_words     = 0;   // number of uint64_t words in bitmap
    std::uint64_t page_count       = 0;   // total data pages
    std::uint64_t free_page_count  = 0;   // maintained by page allocator
    std::uint64_t next_page_cursor = 0;   // allocation hint
    std::uint64_t current_epoch    = 1;   // monotonically increasing
    std::uint64_t delayed_large_head = 0;
    std::uint64_t delayed_large_tail = 0;
    std::uint32_t class_count      = 0;
    std::uint32_t reserved         = 0;
    SizeClassMeta classes[kMaxSizeClasses];
};

// ============================================================
// Thread-local cache (process-local, never persisted)
// ============================================================
struct TlcSlot {
    std::array<std::uint64_t, kTlcCapacity> offsets{};
    std::uint32_t count = 0;
};
struct ThreadLocalCache {
    std::array<TlcSlot, kMaxSizeClasses> slots{};
};
thread_local ThreadLocalCache tl_cache;

// ============================================================
// Helpers
// ============================================================
static std::uint64_t AlignUp(std::uint64_t v, std::uint64_t a) {
    return (v + a - 1) / a * a;
}
static bool IsPowerOfTwo(std::uint64_t v) {
    return v != 0 && (v & (v - 1)) == 0;
}

// Lock-order contract (must never be violated to prevent deadlock):
//   page_mutex > class_mutexes[i]   (page_mutex is never taken while
//                                    holding a class mutex)
//   class_mutexes[i] > large_delay_mutex  (same rule)
//
// ConditionalLock: when concurrent == false (SingleWriter mode) the lock
// acquisition is a no-op, eliminating all synchronisation overhead.
struct ConditionalLock {
    ConditionalLock(std::mutex& m, bool concurrent) : m_(&m), active_(concurrent) {
        if (active_) m_->lock();
    }
    ~ConditionalLock() { if (active_) m_->unlock(); }
    ConditionalLock(const ConditionalLock&) = delete;
    std::mutex* m_;
    bool active_;
};

// ============================================================
// BuildSizeClasses – block sizes (header included)
// ============================================================
std::vector<std::uint32_t> BuildSizeClasses(std::uint32_t page_size) {
    std::vector<std::uint32_t> v;
    // Tiny: 32, 48, 64, …, 320  (step 16)
    for (std::uint32_t s = kMinBlockSize; s <= kMaxTinyTotal; s += kTinyStep)
        v.push_back(s);
    // Middle
    for (std::uint32_t s : {384u, 512u, 768u, 1024u, 1536u, 2048u, 3072u, page_size}) {
        if (v.empty() || v.back() < s) v.push_back(s);  
    }
    if (v.size() > kMaxSizeClasses)
        throw std::logic_error("too many size classes");
    return v;
}

// O(1) tiny-range lookup.
// tiny classes: block_size = 32, 48, 64 … = kMinBlockSize + i*kTinyStep
// For total_needed (= kHeaderSize + user_size) in [kHeaderSize+1, kMaxTinyTotal]:
//   class_id = ceil((total_needed - kMinBlockSize) / kTinyStep)
//            = (total_needed - kMinBlockSize + kTinyStep - 1) / kTinyStep
inline std::int32_t find_class_tiny(std::uint64_t total_needed) noexcept {
    if (total_needed > kMaxTinyTotal) return -1;
    if (total_needed <= kMinBlockSize) return 0;
    return static_cast<std::int32_t>(
        (total_needed - kMinBlockSize + kTinyStep - 1) / kTinyStep);
}

// ============================================================
// MappedFile – thin RAII wrapper around mmap / MapViewOfFile
// ============================================================
class MappedFile {
public:
    MappedFile() = default;
    ~MappedFile() { Close(); }
    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;

    void Open(const AllocatorOptions& opts) {
        Close();
        if (opts.arena_size < 1024 * 1024)
            throw std::invalid_argument("arena_size must be >= 1 MiB");
        if (opts.path.empty()) {
            path_.clear();
            base_ = ::mmap(nullptr, opts.arena_size,
                PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (base_ == MAP_FAILED) {
                base_ = nullptr;
                throw std::system_error(errno, std::generic_category(), "mmap");
            }
            virtual_size_ = opts.arena_size;
            return;
        }

        path_ = opts.path;
        virtual_size_ = opts.max_arena_size ? opts.max_arena_size : opts.arena_size;
        if (virtual_size_ < opts.arena_size)
            throw std::invalid_argument("max_arena_size must be >= arena_size");
        const bool grow_enabled = virtual_size_ > opts.arena_size;
        if (!grow_enabled) {
            segment_size_ = opts.arena_size;
        } else {
            segment_size_ = opts.segment_size ? opts.segment_size : (1ull << 30);
            if (segment_size_ < opts.arena_size) segment_size_ = opts.arena_size;
        }

        // Reserve one continuous virtual range so base+offset addressing remains valid.
        base_ = ::mmap(nullptr, virtual_size_, PROT_NONE,
            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (base_ == MAP_FAILED) {
            base_ = nullptr;
            throw std::system_error(errno, std::generic_category(), "mmap reserve");
        }

        map_segment(/*seg_idx=*/0, opts.create_if_missing);
        // Reopen path may already have additional segment files.
        for (std::uint64_t i = 1; i * segment_size_ < virtual_size_; ++i) {
            const std::string p = seg_path(i);
            if (::access(p.c_str(), F_OK) != 0) break;
            map_segment(i, /*allow_create=*/false);
        }
    }

    void Close() noexcept {
        for (int fd : seg_fds_) {
            if (fd >= 0) ::close(fd);
        }
        seg_fds_.clear();
        seg_mapped_.clear();
        if (base_) { ::munmap(base_, virtual_size_); base_ = nullptr; }
        virtual_size_ = 0;
        segment_size_ = 0;
        path_.clear();
    }

    void EnsureMapped(std::uint64_t offset, std::uint64_t bytes) {
        if (!base_ || path_.empty() || bytes == 0) return;
        const std::uint64_t end_off = offset + bytes - 1;
        const std::uint64_t s0 = offset / segment_size_;
        const std::uint64_t s1 = end_off / segment_size_;
        for (std::uint64_t s = s0; s <= s1; ++s) {
            if (s >= seg_mapped_.size() || !seg_mapped_[s]) map_segment(s, true);
        }
    }

    void*         data() const noexcept { return base_; }
    std::uint64_t size() const noexcept { return virtual_size_; }

private:
    std::string seg_path(std::uint64_t seg_idx) const {
        if (seg_idx == 0) return path_;
        return path_ + ".seg" + std::to_string(seg_idx);
    }

    void map_segment(std::uint64_t seg_idx, bool allow_create) {
        if (!base_) throw std::runtime_error("mapping is not reserved");
        if (seg_idx * segment_size_ >= virtual_size_)
            throw std::bad_alloc();

        if (path_.empty()) return;
        if (seg_idx >= seg_fds_.size()) {
            seg_fds_.resize(seg_idx + 1, -1);
            seg_mapped_.resize(seg_idx + 1, false);
        }
        if (seg_mapped_[seg_idx]) return;

        const std::string p = seg_path(seg_idx);
        int flags = allow_create ? (O_RDWR | O_CREAT) : O_RDWR;
        int fd = ::open(p.c_str(), flags, 0644);
        if (fd < 0)
            throw std::system_error(errno, std::generic_category(), "open segment");

        const std::uint64_t seg_off = seg_idx * segment_size_;
        const std::uint64_t map_len = std::min<std::uint64_t>(
            segment_size_, virtual_size_ - seg_off);
        if (::ftruncate(fd, static_cast<off_t>(map_len)) != 0) {
            ::close(fd);
            throw std::system_error(errno, std::generic_category(), "ftruncate segment");
        }

        void* want = static_cast<char*>(base_) + seg_off;
        void* got = ::mmap(want, map_len, PROT_READ | PROT_WRITE,
                           MAP_SHARED | MAP_FIXED, fd, 0);
        if (got == MAP_FAILED) {
            ::close(fd);
            throw std::system_error(errno, std::generic_category(), "mmap segment");
        }

        seg_fds_[seg_idx] = fd;
        seg_mapped_[seg_idx] = true;
    }

    std::string path_;
    void* base_ = nullptr;
    std::uint64_t virtual_size_ = 0;
    std::uint64_t segment_size_ = 0;
    std::vector<int> seg_fds_;
    std::vector<bool> seg_mapped_;
};

}  // namespace

// ============================================================
// FtAllocator::Impl
// ============================================================
struct FtAllocator::Impl {
    MappedFile mapping;
    AllocatorMetadata* meta = nullptr;
    std::vector<std::uint32_t> class_sizes;
    bool concurrent = true;   // false in SingleWriter mode

    // Time-based reclamation: blocks Delayed-freed before this cutoff may be
    // reclaimed. Configured from AllocatorOptions::reclaim_delay_ns.
    std::uint64_t reclaim_delay_ns = 5'000'000'000ULL;

    // Throttle: only run a reclaim sweep every N Malloc calls to amortise cost.
    static constexpr std::uint32_t kReclaimInterval = 256;
    std::uint32_t malloc_counter = 0;

    // In-memory atomic stats – eliminated P0 data-race.
    // These are reset on each Open() because they cannot be recovered from
    // the mmap file without a full scan.
    std::atomic<std::uint64_t> stat_used_bytes{0};
    std::atomic<std::uint64_t> stat_alloc_count{0};
    std::atomic<std::uint64_t> stat_free_count{0};
    std::atomic<std::uint64_t> stat_delayed_count{0};

    // Locking – lock order:  page_mutex > class_mutexes[i] > large_delay_mutex
    mutable std::mutex page_mutex;
    mutable std::mutex large_delay_mutex;
    mutable std::array<std::mutex, kMaxSizeClasses> class_mutexes;

    // ---- Address helpers ------------------------------------------------
    std::uint8_t* base() const noexcept {
        return static_cast<std::uint8_t*>(mapping.data());
    }
    std::uint64_t* bitmap_words_ptr() const noexcept {
        return reinterpret_cast<std::uint64_t*>(base() + meta->bitmap_offset);
    }
    BlockHeader* header_at(std::uint64_t offset) const noexcept {
        return reinterpret_cast<BlockHeader*>(base() + offset);
    }

    // ---- Initialisation -------------------------------------------------
    void initialize(const AllocatorOptions& opts) {
        if (!IsPowerOfTwo(opts.page_size))
            throw std::invalid_argument("page_size must be a power of two");

        concurrent = (opts.mode == AllocatorMode::Concurrent);
        reclaim_delay_ns = opts.reclaim_delay_ns;
        mapping.Open(opts);
        meta = reinterpret_cast<AllocatorMetadata*>(mapping.data());

        // Reopen existing arena
        if (meta->magic == kMagic && meta->version == kVersion) {
            class_sizes = BuildSizeClasses(meta->page_size);
            if (meta->arena_size != mapping.size() ||
                meta->class_count != static_cast<std::uint32_t>(class_sizes.size()))
                throw std::runtime_error(
                    "existing allocator metadata does not match requested layout");
            // Discard stale TLC entries from any previous allocator on this thread.
            for (auto& slot : tl_cache.slots) slot.count = 0;

            // Recompute free_page_count from bitmap so stats are consistent
            // after a crash (bitmap is the source of truth).
            std::uint64_t fp = 0;
            const std::uint64_t* bm = bitmap_words_ptr();
            const std::uint64_t words = meta->bitmap_words;
            const std::uint64_t last_partial = meta->page_count % 64;
            for (std::uint64_t w = 0; w < words; ++w) {
                std::uint64_t word = bm[w];
                if (last_partial && w == words - 1) {
                    // Only count bits for valid pages in the last word
                    word &= (1ULL << last_partial) - 1;
                }
                fp += static_cast<std::uint64_t>(__builtin_popcountll(~word &
                    (last_partial && w == words - 1
                        ? (1ULL << last_partial) - 1
                        : ~0ULL)));
            }
            meta->free_page_count = fp;

            // On recovery, all delayed blocks from the previous process run
            // must be reclaimed immediately: no readers from that process exist
            // any more.  Use cutoff = UINT64_MAX so every block qualifies.
            reclaim_with_cutoff(std::numeric_limits<std::uint64_t>::max());
            return;
        }

        // Fresh arena: zero only the header + bitmap area (P2 optimisation:
        // avoid zeroing the entire data region which the OS provides zeroed
        // on demand anyway).
        class_sizes = BuildSizeClasses(opts.page_size);

        meta->magic      = kMagic;
        meta->version    = kVersion;
        meta->page_size  = opts.page_size;
        meta->arena_size = mapping.size();
        meta->class_count = static_cast<std::uint32_t>(class_sizes.size());

        const std::uint64_t meta_bytes =
            AlignUp(sizeof(AllocatorMetadata), opts.page_size);
        const std::uint64_t rough_pages =
            (mapping.size() - meta_bytes) / opts.page_size;
        const std::uint64_t bm_words =
            AlignUp((rough_pages + 63) / 64, 1);            // ceil to full word
        const std::uint64_t bm_bytes =
            AlignUp(bm_words * sizeof(std::uint64_t), opts.page_size);

        meta->bitmap_offset = meta_bytes;
        meta->bitmap_words  = bm_words;
        meta->data_offset   = meta_bytes + bm_bytes;
        meta->page_count    = (mapping.size() - meta->data_offset) / opts.page_size;
        meta->free_page_count = meta->page_count;
        meta->current_epoch = 1;

        // Zero header + bitmap (data pages rely on OS zero-fill).
        std::memset(mapping.data(), 0,
            static_cast<std::size_t>(meta->data_offset));

        // Re-apply magic & layout fields (memset cleared them).
        meta->magic       = kMagic;
        meta->version     = kVersion;
        meta->page_size   = opts.page_size;
        meta->arena_size  = mapping.size();
        meta->class_count = static_cast<std::uint32_t>(class_sizes.size());
        meta->bitmap_offset = meta_bytes;
        meta->bitmap_words  = bm_words;
        meta->data_offset   = meta_bytes + bm_bytes;
        meta->page_count    = (mapping.size() - meta->data_offset) / opts.page_size;
        meta->free_page_count = meta->page_count;
        meta->current_epoch   = 1;

        // Discard any stale TLC entries left by a previous allocator instance
        // on this thread; their offsets would be invalid for the new arena.
        for (auto& slot : tl_cache.slots) slot.count = 0;

        for (std::size_t i = 0; i < class_sizes.size(); ++i) {
            meta->classes[i].block_size    = class_sizes[i];
            meta->classes[i].pages_per_slab = kDefaultSlabPages;
        }
    }

    // ---- Pointer / offset conversion ------------------------------------
    std::uint64_t ptr_to_offset(const void* ptr) const {
        if (!ptr) return 0;
        const auto* p = static_cast<const std::uint8_t*>(ptr);
        const auto* b = base();
        if (p < b || p >= b + mapping.size())
            throw std::invalid_argument("pointer outside mmap arena");
        return static_cast<std::uint64_t>(p - b);
    }
    void* offset_to_ptr(std::uint64_t off) const {
        if (!off) return nullptr;
        if (off >= mapping.size())
            throw std::out_of_range("offset outside mmap arena");
        return base() + off;
    }
    BlockHeader* header_from_payload(void* payload) const {
        auto* hdr = reinterpret_cast<BlockHeader*>(
            static_cast<char*>(payload) - kHeaderSize);
        if (hdr->magic != kBlockMagic)
            throw std::runtime_error("invalid allocation header (corrupt magic)");
        return hdr;
    }

    // ---- O(1) size → class lookup (P1) ---------------------------------
    std::int32_t find_class(std::size_t user_size) const noexcept {
        const std::uint64_t total = static_cast<std::uint64_t>(user_size) + kHeaderSize;
        const std::int32_t tiny_id = find_class_tiny(total);
        if (tiny_id >= 0) return tiny_id;
        // Linear scan of middle classes (only ~8 entries)
        const std::uint32_t tiny_count =
            static_cast<std::uint32_t>((kMaxTinyTotal - kMinBlockSize) / kTinyStep + 1);
        for (std::uint32_t i = tiny_count; i < meta->class_count; ++i) {
            if (meta->classes[i].block_size >= total)
                return static_cast<std::int32_t>(i);
        }
        return -1;  // large object
    }

    // ---- Page bitmap (uint64_t words, P2) --------------------------------
    // Bit j in word w = page (w*64 + j); 1 = used, 0 = free.
    void bitmap_set(std::uint64_t page) noexcept {
        bitmap_words_ptr()[page / 64] |= (1ULL << (page % 64));
    }
    void bitmap_clear(std::uint64_t page) noexcept {
        bitmap_words_ptr()[page / 64] &= ~(1ULL << (page % 64));
    }
    bool bitmap_test(std::uint64_t page) const noexcept {
        return (bitmap_words_ptr()[page / 64] >> (page % 64)) & 1u;
    }

    std::uint64_t alloc_pages(std::uint64_t count) {
        ConditionalLock guard(page_mutex, concurrent);
        if (!count || count > meta->free_page_count) throw std::bad_alloc();

        std::uint64_t* bm   = bitmap_words_ptr();
        const std::uint64_t words = meta->bitmap_words;
        const std::uint64_t cur   = meta->next_page_cursor;

        if (count <= 64) {
            // Fast path: try to fit 'count' pages inside one 64-page word.
            const std::uint64_t start_w = (cur / 64) % words;
            const std::uint64_t mask =
                (count == 64) ? ~0ULL : ((1ULL << count) - 1);

            for (std::uint64_t ws = 0; ws < words; ++ws) {
                const std::uint64_t w = (start_w + ws) % words;
                if (bm[w] == ~0ULL) continue;  // word fully used
                const std::uint64_t free = ~bm[w];

                for (std::uint32_t shift = 0; shift + count <= 64; ++shift) {
                    const std::uint64_t page = w * 64 + shift;
                    if (page >= meta->page_count) break;
                    if (page + count > meta->page_count) break;
                    if ((free & (mask << shift)) == (mask << shift)) {
                        bm[w] |= (mask << shift);
                        meta->free_page_count -= count;
                        meta->next_page_cursor = (page + count) % meta->page_count;
                        const std::uint64_t off = meta->data_offset + page * meta->page_size;
                        mapping.EnsureMapped(off, count * meta->page_size);
                        return off;
                    }
                }
            }
        }

        // General path: scan page by page (for large count or cross-word spans).
        for (std::uint64_t s = 0; s < meta->page_count; ++s) {
            const std::uint64_t start = (cur + s) % meta->page_count;
            if (start + count > meta->page_count) continue;

            bool ok = true;
            for (std::uint64_t j = 0; j < count; ++j) {
                if (bitmap_test(start + j)) { ok = false; s += j; break; }
            }
            if (!ok) continue;

            for (std::uint64_t j = 0; j < count; ++j) bitmap_set(start + j);
            meta->free_page_count -= count;
            meta->next_page_cursor = (start + count) % meta->page_count;
            const std::uint64_t off = meta->data_offset + start * meta->page_size;
            mapping.EnsureMapped(off, count * meta->page_size);
            return off;
        }
        throw std::bad_alloc();
    }

    void free_pages(std::uint64_t offset, std::uint64_t count) {
        ConditionalLock guard(page_mutex, concurrent);
        if (offset < meta->data_offset ||
            (offset - meta->data_offset) % meta->page_size)
            throw std::runtime_error("invalid page offset");
        const std::uint64_t start =
            (offset - meta->data_offset) / meta->page_size;
        if (start + count > meta->page_count)
            throw std::runtime_error("page free range outside arena");
        for (std::uint64_t j = 0; j < count; ++j) {
            if (!bitmap_test(start + j))
                throw std::runtime_error("double free in page bitmap");
            bitmap_clear(start + j);
        }
        meta->free_page_count += count;
    }

    // ---- Freelist helpers (use FreeChain inside payload, P1) ------------
    void push_free(SizeClassMeta& klass, BlockHeader* hdr,
                   std::uint16_t class_id) noexcept {
        hdr->flags    = 0;
        hdr->class_id = class_id;
        auto* fc = get_free_chain(hdr);
        fc->next_offset = klass.free_head;
        fc->free_time_ns = 0;
        klass.free_head = ptr_to_offset(hdr);
    }

    BlockHeader* pop_free(SizeClassMeta& klass) noexcept {
        if (!klass.free_head) return nullptr;
        auto* hdr       = header_at(klass.free_head);
        klass.free_head = get_free_chain(hdr)->next_offset;
        get_free_chain(hdr)->next_offset = 0;
        return hdr;
    }

    // Expand a size-class freelist by allocating a new slab of pages.
    // Called while holding class_mutexes[class_id].
    // Lock order: class_mutex is held here; alloc_pages acquires page_mutex.
    // page_mutex is never acquired while holding a class_mutex from OUTSIDE
    // fill_class – see lock-order comment at ConditionalLock.
    void fill_class(std::uint16_t class_id) {
        SizeClassMeta& klass = meta->classes[class_id];
        const std::uint64_t slab_offset = alloc_pages(klass.pages_per_slab);
        const std::uint64_t slab_bytes  =
            static_cast<std::uint64_t>(klass.pages_per_slab) * meta->page_size;
        const std::uint64_t count = slab_bytes / klass.block_size;
        if (!count) throw std::bad_alloc();
        for (std::uint64_t i = 0; i < count; ++i) {
            auto* hdr = reinterpret_cast<BlockHeader*>(
                base() + slab_offset + i * klass.block_size);
            *hdr = BlockHeader{};
            hdr->magic      = kBlockMagic;
            hdr->block_size = klass.block_size;
            push_free(klass, hdr, class_id);
        }
    }

    // ---- Thread-local cache helpers (P2 / SWMR) -------------------------
    // Flush all TLC slots for a specific class back to the shared freelist.
    void flush_tlc_class(std::uint16_t class_id) {
        TlcSlot& slot = tl_cache.slots[class_id];
        if (!slot.count) return;
        SizeClassMeta& klass = meta->classes[class_id];
        ConditionalLock guard(class_mutexes[class_id], concurrent);
        while (slot.count) {
            std::uint64_t off = slot.offsets[--slot.count];
            auto* hdr = header_at(off);
            auto* fc  = get_free_chain(hdr);
            fc->next_offset = klass.free_head;
            fc->free_time_ns = 0;
            hdr->flags      = 0;
            klass.free_head = off;
        }
    }

    // ---- Small / middle allocation (with TLC) ---------------------------
    void* malloc_small(std::size_t /*size*/, std::uint16_t class_id) {
        TlcSlot& slot = tl_cache.slots[class_id];

        // Fast path: TLC hit (no lock).
        if (slot.count) {
            std::uint64_t off = slot.offsets[--slot.count];
            auto* hdr = header_at(off);
            hdr->flags         = kFlagAllocated;
            hdr->class_id      = class_id;
            hdr->block_size    = meta->classes[class_id].block_size;
            stat_alloc_count.fetch_add(1, std::memory_order_relaxed);
            stat_used_bytes.fetch_add(hdr->block_size, std::memory_order_relaxed);
            return reinterpret_cast<char*>(hdr) + kHeaderSize;
        }

        // Refill TLC from central freelist.
        SizeClassMeta& klass = meta->classes[class_id];
        {
            ConditionalLock guard(class_mutexes[class_id], concurrent);
            if (!klass.free_head) fill_class(class_id);
            std::uint32_t n = 0;
            while (n < kTlcCapacity / 2 && klass.free_head) {
                slot.offsets[slot.count++] = klass.free_head;
                auto* hdr       = header_at(klass.free_head);
                klass.free_head = get_free_chain(hdr)->next_offset;
                ++n;
            }
        }
        if (!slot.count) throw std::bad_alloc();

        std::uint64_t off = slot.offsets[--slot.count];
        auto* hdr = header_at(off);
        hdr->magic         = kBlockMagic;
        hdr->flags         = kFlagAllocated;
        hdr->class_id      = class_id;
        hdr->block_size    = klass.block_size;
        hdr->page_count    = 0;
        stat_alloc_count.fetch_add(1, std::memory_order_relaxed);
        stat_used_bytes.fetch_add(hdr->block_size, std::memory_order_relaxed);
        return reinterpret_cast<char*>(hdr) + kHeaderSize;
    }

    void* malloc_large(std::size_t size) {
        const std::uint64_t total =
            AlignUp(kHeaderSize + size, meta->page_size);
        const std::uint64_t pages = total / meta->page_size;
        const std::uint64_t off   = alloc_pages(pages);
        auto* hdr = reinterpret_cast<BlockHeader*>(base() + off);
        *hdr = BlockHeader{};
        hdr->magic      = kBlockMagic;
        hdr->class_id   = kLargeClassId;
        hdr->flags      = kFlagAllocated;
        hdr->block_size = static_cast<std::uint32_t>(
            std::min<std::uint64_t>(total, std::numeric_limits<std::uint32_t>::max()));
        hdr->page_count = static_cast<std::uint32_t>(pages);
        stat_alloc_count.fetch_add(1, std::memory_order_relaxed);
        stat_used_bytes.fetch_add(total, std::memory_order_relaxed);
        return reinterpret_cast<char*>(hdr) + kHeaderSize;
    }

    // ---- Immediate free --------------------------------------------------
    void free_small_immediate(BlockHeader* hdr) {
        const std::uint16_t class_id = hdr->class_id;
        stat_free_count.fetch_add(1, std::memory_order_relaxed);
        stat_used_bytes.fetch_sub(hdr->block_size, std::memory_order_relaxed);

        // Try TLC first (no lock).
        TlcSlot& slot = tl_cache.slots[class_id];
        if (slot.count < kTlcCapacity) {
            hdr->flags = 0;
            slot.offsets[slot.count++] = ptr_to_offset(hdr);
            return;
        }

        // TLC full: flush half to central freelist, then add to TLC.
        SizeClassMeta& klass = meta->classes[class_id];
        {
            ConditionalLock guard(class_mutexes[class_id], concurrent);
            const std::uint32_t flush = kTlcCapacity / 2;
            for (std::uint32_t i = 0; i < flush; ++i) {
                std::uint64_t o = slot.offsets[--slot.count];
                auto* h   = header_at(o);
                auto* fc  = get_free_chain(h);
                fc->next_offset = klass.free_head;
                fc->free_time_ns = 0;
                h->flags        = 0;
                klass.free_head = o;
            }
        }
        hdr->flags = 0;
        slot.offsets[slot.count++] = ptr_to_offset(hdr);
    }

    void free_large_immediate(BlockHeader* hdr) {
        const std::uint64_t pages = hdr->page_count;
        const std::uint64_t off   = ptr_to_offset(hdr);
        stat_free_count.fetch_add(1, std::memory_order_relaxed);
        stat_used_bytes.fetch_sub(
            static_cast<std::uint64_t>(pages) * meta->page_size,
            std::memory_order_relaxed);
        hdr->flags = 0;
        free_pages(off, pages);
    }

    // ---- Delayed free (time-based) --------------------------------------
    void free_small_delayed(BlockHeader* hdr) {
        const std::uint16_t class_id = hdr->class_id;
        const std::uint64_t ts = steady_ns();

        SizeClassMeta& klass = meta->classes[class_id];
        ConditionalLock guard(class_mutexes[class_id], concurrent);
        hdr->flags = kFlagDelayed;
        auto* fc = get_free_chain(hdr);
        fc->next_offset  = 0;
        fc->free_time_ns = ts;
        const std::uint64_t off = ptr_to_offset(hdr);
        if (!klass.delayed_tail) {
            klass.delayed_head = off;
        } else {
            get_free_chain(header_at(klass.delayed_tail))->next_offset = off;
        }
        klass.delayed_tail = off;
        stat_free_count.fetch_add(1, std::memory_order_relaxed);
        stat_delayed_count.fetch_add(1, std::memory_order_relaxed);
        stat_used_bytes.fetch_sub(hdr->block_size, std::memory_order_relaxed);
    }

    void free_large_delayed(BlockHeader* hdr) {
        const std::uint64_t ts = steady_ns();

        ConditionalLock guard(large_delay_mutex, concurrent);
        hdr->flags = kFlagDelayed;
        auto* fc = get_free_chain(hdr);
        fc->next_offset  = 0;
        fc->free_time_ns = ts;
        const std::uint64_t off = ptr_to_offset(hdr);
        if (!meta->delayed_large_tail) {
            meta->delayed_large_head = off;
        } else {
            get_free_chain(header_at(meta->delayed_large_tail))->next_offset = off;
        }
        meta->delayed_large_tail = off;
        stat_free_count.fetch_add(1, std::memory_order_relaxed);
        stat_delayed_count.fetch_add(1, std::memory_order_relaxed);
        stat_used_bytes.fetch_sub(
            static_cast<std::uint64_t>(hdr->page_count) * meta->page_size,
            std::memory_order_relaxed);
    }

    // ---- Time-based reclamation -----------------------------------------

    // Core sweep: reclaim all Delayed blocks whose free_time_ns <= cutoff.
    // cutoff = steady_ns() - reclaim_delay_ns  for normal use;
    // cutoff = UINT64_MAX                       to force-reclaim everything.
    std::size_t reclaim_with_cutoff(std::uint64_t cutoff) {
        std::size_t reclaimed = 0;

        // Per-class small blocks.
        for (std::uint16_t cid = 0;
             cid < static_cast<std::uint16_t>(meta->class_count); ++cid) {
            SizeClassMeta& klass = meta->classes[cid];
            ConditionalLock guard(class_mutexes[cid], concurrent);
            while (klass.delayed_head) {
                auto* hdr = header_at(klass.delayed_head);
                auto* fc  = get_free_chain(hdr);
                if (fc->free_time_ns > cutoff) break;
                klass.delayed_head = fc->next_offset;
                if (!klass.delayed_head) klass.delayed_tail = 0;
                push_free(klass, hdr, cid);
                stat_delayed_count.fetch_sub(1, std::memory_order_relaxed);
                ++reclaimed;
            }
        }

        // Large blocks.
        {
            ConditionalLock guard(large_delay_mutex, concurrent);
            while (meta->delayed_large_head) {
                auto* hdr = header_at(meta->delayed_large_head);
                auto* fc  = get_free_chain(hdr);
                if (fc->free_time_ns > cutoff) break;
                const std::uint64_t next = fc->next_offset;
                const std::uint64_t off  = ptr_to_offset(hdr);
                meta->delayed_large_head = next;
                if (!next) meta->delayed_large_tail = 0;
                hdr->flags = 0;
                fc->next_offset = 0;
                stat_delayed_count.fetch_sub(1, std::memory_order_relaxed);
                free_pages(off, hdr->page_count);
                ++reclaimed;
            }
        }
        return reclaimed;
    }

    // Called from Malloc() every kReclaimInterval allocations.
    void maybe_reclaim_expired() {
        if (++malloc_counter < kReclaimInterval) return;
        malloc_counter = 0;
        const std::uint64_t now = steady_ns();
        if (now < reclaim_delay_ns) return;  // clock not advanced enough yet
        reclaim_with_cutoff(now - reclaim_delay_ns);
    }
};

// ============================================================
// FtAllocator – public interface
// ============================================================
FtAllocator::FtAllocator() : impl_(std::make_unique<Impl>()) {}

FtAllocator::FtAllocator(const AllocatorOptions& opts) : FtAllocator() {
    Open(opts);
}

FtAllocator::~FtAllocator() = default;
FtAllocator::FtAllocator(FtAllocator&&) noexcept = default;
FtAllocator& FtAllocator::operator=(FtAllocator&&) noexcept = default;

void FtAllocator::Open(const AllocatorOptions& opts) {
    if (!impl_) impl_ = std::make_unique<Impl>();
    impl_->initialize(opts);
}

void FtAllocator::Close() noexcept {
    if (impl_) {
        impl_->mapping.Close();
        impl_->meta = nullptr;
    }
}

bool FtAllocator::IsOpen() const noexcept {
    return impl_ && impl_->meta;
}

void* FtAllocator::BaseAddress() const noexcept {
    return IsOpen() ? impl_->mapping.data() : nullptr;
}

void* FtAllocator::Malloc(std::size_t size) {
    if (!IsOpen()) throw std::runtime_error("allocator not open");
    const std::int32_t cid = impl_->find_class(size);
    void* ptr = (cid >= 0) ? impl_->malloc_small(size, static_cast<std::uint16_t>(cid))
                           : impl_->malloc_large(size);
    impl_->maybe_reclaim_expired();
    return ptr;
}

void FtAllocator::Free(void* ptr, FreeMode mode) {
    if (!ptr) return;
    if (!IsOpen()) throw std::runtime_error("allocator not open");

    BlockHeader* hdr = impl_->header_from_payload(ptr);
    if (!(hdr->flags & kFlagAllocated))
        throw std::runtime_error("double free detected");

    const bool large = (hdr->class_id == kLargeClassId);
    if (mode == FreeMode::Delayed) {
        large ? impl_->free_large_delayed(hdr)
              : impl_->free_small_delayed(hdr);
    } else {
        large ? impl_->free_large_immediate(hdr)
              : impl_->free_small_immediate(hdr);
    }
}

std::uint64_t FtAllocator::PtrToOffset(const void* ptr) const {
    if (!IsOpen()) throw std::runtime_error("allocator not open");
    return impl_->ptr_to_offset(ptr);
}

void* FtAllocator::OffsetToPtr(std::uint64_t off) const {
    if (!IsOpen()) throw std::runtime_error("allocator not open");
    return impl_->offset_to_ptr(off);
}

std::uint64_t FtAllocator::AdvanceEpoch() {
    if (!IsOpen()) throw std::runtime_error("allocator not open");
    std::uint64_t e = __atomic_add_fetch(
        &impl_->meta->current_epoch, 1, __ATOMIC_ACQ_REL);
    return e;
}

std::uint64_t FtAllocator::CurrentEpoch() const {
    if (!IsOpen()) throw std::runtime_error("allocator not open");
    return __atomic_load_n(&impl_->meta->current_epoch, __ATOMIC_ACQUIRE);
}

// SingleWriter: release fence so readers see all mutations done before this.
void FtAllocator::PublishFence() noexcept {
    std::atomic_thread_fence(std::memory_order_release);
}

std::size_t FtAllocator::ReclaimExpired() {
    if (!IsOpen()) throw std::runtime_error("allocator not open");
    const std::uint64_t now = steady_ns();
    if (now < impl_->reclaim_delay_ns) return 0;
    return impl_->reclaim_with_cutoff(now - impl_->reclaim_delay_ns);
}

std::uint64_t FtAllocator::ReclaimDelayNs() const noexcept {
    return impl_ ? impl_->reclaim_delay_ns : 5'000'000'000ULL;
}

void FtAllocator::FlushTlc() {
    if (!IsOpen()) return;
    for (std::uint16_t cid = 0;
         cid < static_cast<std::uint16_t>(impl_->meta->class_count); ++cid) {
        impl_->flush_tlc_class(cid);
    }
}

AllocatorStats FtAllocator::GetStats() const {
    if (!IsOpen()) throw std::runtime_error("allocator not open");
    AllocatorStats s;
    s.arena_size       = impl_->meta->arena_size;
    s.data_offset      = impl_->meta->data_offset;
    s.page_size        = impl_->meta->page_size;
    s.page_count       = impl_->meta->page_count;
    s.free_page_count  = impl_->meta->free_page_count;
    s.used_bytes       = impl_->stat_used_bytes.load(std::memory_order_relaxed);
    s.allocation_count = impl_->stat_alloc_count.load(std::memory_order_relaxed);
    s.free_count       = impl_->stat_free_count.load(std::memory_order_relaxed);
    s.delayed_count    = impl_->stat_delayed_count.load(std::memory_order_relaxed);
    s.current_epoch    = __atomic_load_n(&impl_->meta->current_epoch, __ATOMIC_ACQUIRE);
    return s;
}

void FtAllocator::CheckConsistency() const {
    if (!IsOpen()) throw std::runtime_error("allocator not open");
    if (impl_->meta->magic != kMagic || impl_->meta->version != kVersion)
        throw std::runtime_error("invalid metadata magic/version");
    if (impl_->meta->data_offset >= impl_->meta->arena_size)
        throw std::runtime_error("invalid data offset");

    const std::uint64_t* bm       = impl_->bitmap_words_ptr();
    const std::uint64_t  words    = impl_->meta->bitmap_words;
    const std::uint64_t  pages    = impl_->meta->page_count;
    const std::uint64_t  last_rem = pages % 64;
    std::uint64_t fp = 0;
    for (std::uint64_t w = 0; w < words; ++w) {
        std::uint64_t word = bm[w];
        if (last_rem && w == words - 1) {
            const std::uint64_t valid = (1ULL << last_rem) - 1;
            fp += static_cast<std::uint64_t>(__builtin_popcountll(~word & valid));
        } else {
            fp += static_cast<std::uint64_t>(__builtin_popcountll(~word));
        }
    }
    if (fp != impl_->meta->free_page_count)
        throw std::runtime_error("page bitmap diverged from free_page_count");
}

}  // namespace alloc
}  // namespace yikv
