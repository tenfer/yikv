#include "src/container/bitmap.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace yikv {
namespace container {

using FreeMode = yikv::alloc::FreeMode;

// ============================================================
// Construction / destruction
// ============================================================

Bitmap::Bitmap(Allocator* alloc, uint64_t root_off)
    : alloc_(alloc), base_(alloc->BaseAddress()), root_off_(root_off) {
    if (root_off_ == 0) {
        void* mem = alloc_->Malloc(sizeof(bm_detail::BmRoot));
        auto* r   = static_cast<bm_detail::BmRoot*>(mem);
        r->magic          = bm_detail::kBmMagic;
        r->version        = bm_detail::kBmVersion;
        r->_pad           = 0;
        r->cardinality    = 0;
        r->chunk_count    = 0;
        r->chunk_capacity = 0;
        r->chunks_off     = 0;
        root_off_ = off_of(r);
    } else {
        const auto* r = root();
        if (r->magic != bm_detail::kBmMagic)
            throw std::runtime_error("Bitmap: bad magic on recovery");
        if (r->version != bm_detail::kBmVersion)
            throw std::runtime_error("Bitmap: unsupported version on recovery");
    }
}

Bitmap::Bitmap(const Bitmap& other)
    : alloc_(other.alloc_), base_(other.base_), root_off_(0) {
    // Allocate a fresh BmRoot in the same arena, then copy other's content.
    void* mem = alloc_->Malloc(sizeof(bm_detail::BmRoot));
    auto* r   = static_cast<bm_detail::BmRoot*>(mem);
    r->magic          = bm_detail::kBmMagic;
    r->version        = bm_detail::kBmVersion;
    r->_pad           = 0;
    r->cardinality    = 0;
    r->chunk_count    = 0;
    r->chunk_capacity = 0;
    r->chunks_off     = 0;
    root_off_ = off_of(r);
    OrWith(other);
}

Bitmap& Bitmap::operator=(const Bitmap& other) {
    if (this == &other) return *this;
    // Clear existing chunks, then re-populate from other.
    auto* r = root();
    for (uint32_t ci = 0; ci < r->chunk_count; ++ci) {
        auto& e = idx()[ci];
        if (e.payload_off)
            alloc_->Free(alloc_->OffsetToPtr(e.payload_off), FreeMode::Delayed);
    }
    if (r->chunks_off)
        alloc_->Free(alloc_->OffsetToPtr(r->chunks_off), FreeMode::Delayed);
    r->cardinality    = 0;
    r->chunk_count    = 0;
    r->chunk_capacity = 0;
    r->chunks_off     = 0;
    OrWith(other);
    return *this;
}

Bitmap::Bitmap(Bitmap&& o) noexcept
    : alloc_(o.alloc_), base_(o.base_), root_off_(o.root_off_),
      owned_alloc_(std::move(o.owned_alloc_)) {
    o.alloc_    = nullptr;
    o.base_     = nullptr;
    o.root_off_ = 0;
}

Bitmap& Bitmap::operator=(Bitmap&& o) noexcept {
    if (this != &o) {
        alloc_       = o.alloc_;
        base_        = o.base_;
        root_off_    = o.root_off_;
        owned_alloc_ = std::move(o.owned_alloc_);
        o.alloc_     = nullptr;
        o.base_      = nullptr;
        o.root_off_  = 0;
    }
    return *this;
}

// ============================================================
// Chunk index management
// ============================================================

uint32_t Bitmap::chunk_pos(uint16_t hi) const noexcept {
    const auto* r       = root();
    const auto* entries = (r->chunks_off == 0) ? nullptr : idx();
    uint32_t lo = 0, rh = r->chunk_count;
    while (lo < rh) {
        uint32_t mid = (lo + rh) >> 1;
        if (entries[mid].hi < hi) lo = mid + 1;
        else                      rh = mid;
    }
    return lo;
}

bm_detail::BmChunkEntry* Bitmap::find_chunk(uint16_t hi) noexcept {
    const auto* r = root();
    if (r->chunk_count == 0) return nullptr;
    uint32_t pos = chunk_pos(hi);
    auto* entries = idx();
    if (pos < r->chunk_count && entries[pos].hi == hi) return &entries[pos];
    return nullptr;
}

const bm_detail::BmChunkEntry* Bitmap::find_chunk(uint16_t hi) const noexcept {
    const auto* r = root();
    if (r->chunk_count == 0) return nullptr;
    uint32_t pos = chunk_pos(hi);
    const auto* entries = idx();
    if (pos < r->chunk_count && entries[pos].hi == hi) return &entries[pos];
    return nullptr;
}

void Bitmap::ensure_index_cap(uint32_t needed) {
    auto* r = root();
    if (r->chunk_capacity >= needed) return;
    uint32_t new_cap = std::max({r->chunk_capacity * 2, needed, kIndexInitCap});
    void* new_mem = alloc_->Malloc(new_cap * sizeof(bm_detail::BmChunkEntry));
    if (r->chunks_off != 0) {
        std::memcpy(new_mem, idx(), r->chunk_count * sizeof(bm_detail::BmChunkEntry));
        alloc_->Free(alloc_->OffsetToPtr(r->chunks_off), FreeMode::Delayed);
    }
    r->chunks_off     = off_of(new_mem);
    r->chunk_capacity = new_cap;
}

bm_detail::BmChunkEntry* Bitmap::get_or_create(uint16_t hi) {
    // Grow before searching so idx() is stable afterwards.
    ensure_index_cap(root()->chunk_count + 1);
    auto* r       = root();
    auto* entries = idx();
    uint32_t pos  = chunk_pos(hi);
    if (pos < r->chunk_count && entries[pos].hi == hi) return &entries[pos];
    // Insert at pos.
    if (pos < r->chunk_count)
        std::memmove(entries + pos + 1, entries + pos,
                     (r->chunk_count - pos) * sizeof(bm_detail::BmChunkEntry));
    auto& e       = entries[pos];
    void* payload = alloc_->Malloc(kArrayInitCap * sizeof(uint16_t));
    e.hi          = hi;
    e.type        = 0;
    e._pad        = 0;
    e.cardinality = 0;
    e.payload_off = off_of(payload);
    e.payload_len = kArrayInitCap * sizeof(uint16_t);
    e._pad2       = 0;
    r->chunk_count++;
    return &e;
}

void Bitmap::erase_chunk(uint32_t pos) {
    auto* r       = root();
    auto* entries = idx();
    auto& e       = entries[pos];
    if (e.payload_off) {
        alloc_->Free(alloc_->OffsetToPtr(e.payload_off), FreeMode::Delayed);
        e.payload_off = 0;
    }
    if (pos + 1 < r->chunk_count)
        std::memmove(entries + pos, entries + pos + 1,
                     (r->chunk_count - pos - 1) * sizeof(bm_detail::BmChunkEntry));
    r->chunk_count--;
}

// ============================================================
// Mode conversion helpers
// ============================================================

void Bitmap::grow_array(bm_detail::BmChunkEntry& e, uint32_t new_cap_elements) {
    uint32_t new_bytes = new_cap_elements * static_cast<uint32_t>(sizeof(uint16_t));
    void* new_mem = alloc_->Malloc(new_bytes);
    std::memcpy(new_mem, arr(e), e.cardinality * sizeof(uint16_t));
    alloc_->Free(alloc_->OffsetToPtr(e.payload_off), FreeMode::Delayed);
    e.payload_off = off_of(new_mem);
    e.payload_len = new_bytes;
}

void Bitmap::to_bitmap(bm_detail::BmChunkEntry& e) {
    if (e.type == 1) return;
    void* new_mem = alloc_->Malloc(kBitmapWords * sizeof(uint64_t));
    auto* words   = static_cast<uint64_t*>(new_mem);
    std::memset(words, 0, kBitmapWords * sizeof(uint64_t));
    const uint16_t* a = arr(e);
    for (uint32_t i = 0; i < e.cardinality; ++i)
        words[a[i] >> 6] |= uint64_t(1) << (a[i] & 63u);
    alloc_->Free(alloc_->OffsetToPtr(e.payload_off), FreeMode::Delayed);
    e.payload_off = off_of(new_mem);
    e.payload_len = kBitmapWords * static_cast<uint32_t>(sizeof(uint64_t));
    e.type        = 1;
}

void Bitmap::to_array(bm_detail::BmChunkEntry& e) {
    if (e.type == 0) return;
    uint32_t n    = e.cardinality;
    void* new_mem = alloc_->Malloc(n * sizeof(uint16_t));
    auto* out     = static_cast<uint16_t*>(new_mem);
    const uint64_t* b = bmp(e);
    uint32_t k = 0;
    for (uint32_t wi = 0; wi < kBitmapWords; ++wi) {
        uint64_t word = b[wi];
        while (word) {
            uint32_t bit = static_cast<uint32_t>(__builtin_ctzll(word));
            out[k++] = static_cast<uint16_t>((wi << 6) | bit);
            word &= word - 1;
        }
    }
    alloc_->Free(alloc_->OffsetToPtr(e.payload_off), FreeMode::Delayed);
    e.payload_off = off_of(new_mem);
    e.payload_len = n * static_cast<uint32_t>(sizeof(uint16_t));
    e.type        = 0;
}

// ============================================================
// Set-op word helpers
// ============================================================

void Bitmap::chunk_to_words(const bm_detail::BmChunkEntry& e,
                             uint64_t (&w)[kBitmapWords]) const noexcept {
    if (e.type == 1) {
        std::memcpy(w, bmp(e), kBitmapWords * sizeof(uint64_t));
        return;
    }
    std::memset(w, 0, kBitmapWords * sizeof(uint64_t));
    const uint16_t* a = arr(e);
    for (uint32_t i = 0; i < e.cardinality; ++i)
        w[a[i] >> 6] |= uint64_t(1) << (a[i] & 63u);
}

uint32_t Bitmap::popcount_words(const uint64_t (&w)[kBitmapWords]) noexcept {
    uint32_t c = 0;
    for (uint32_t i = 0; i < kBitmapWords; ++i)
        c += static_cast<uint32_t>(__builtin_popcountll(w[i]));
    return c;
}

void Bitmap::build_from_words(bm_detail::BmChunkEntry& e,
                               const uint64_t (&w)[kBitmapWords], uint32_t card) {
    if (card > kArrayToBitmapThreshold) {
        // Ensure bitmap payload.
        if (e.type == 0 || e.payload_len < kBitmapWords * sizeof(uint64_t)) {
            if (e.payload_off)
                alloc_->Free(alloc_->OffsetToPtr(e.payload_off), FreeMode::Delayed);
            void* m   = alloc_->Malloc(kBitmapWords * sizeof(uint64_t));
            e.payload_off = off_of(m);
            e.payload_len = kBitmapWords * static_cast<uint32_t>(sizeof(uint64_t));
        }
        std::memcpy(bmp(e), w, kBitmapWords * sizeof(uint64_t));
        e.type        = 1;
        e.cardinality = card;
    } else {
        // Array mode: collect set bits.
        if (e.payload_len < card * sizeof(uint16_t)) {
            if (e.payload_off)
                alloc_->Free(alloc_->OffsetToPtr(e.payload_off), FreeMode::Delayed);
            void* m   = alloc_->Malloc(card * sizeof(uint16_t));
            e.payload_off = off_of(m);
            e.payload_len = card * static_cast<uint32_t>(sizeof(uint16_t));
        }
        uint16_t* a = arr(e);
        uint32_t  k = 0;
        for (uint32_t wi = 0; wi < kBitmapWords; ++wi) {
            uint64_t word = w[wi];
            while (word) {
                uint32_t bit = static_cast<uint32_t>(__builtin_ctzll(word));
                a[k++] = static_cast<uint16_t>((wi << 6) | bit);
                word &= word - 1;
            }
        }
        e.type        = 0;
        e.cardinality = card;
    }
}

void Bitmap::copy_append_chunk(uint16_t hi,
                               const bm_detail::BmChunkEntry& src,
                               const Bitmap& src_bm) {
    uint32_t bytes = (src.type == 1)
                         ? kBitmapWords * static_cast<uint32_t>(sizeof(uint64_t))
                         : src.cardinality * static_cast<uint32_t>(sizeof(uint16_t));
    auto* te = get_or_create(hi);
    if (te->payload_off)
        alloc_->Free(alloc_->OffsetToPtr(te->payload_off), FreeMode::Delayed);
    void* m = alloc_->Malloc(bytes);
    std::memcpy(m,
                static_cast<const char*>(src_bm.base_) + src.payload_off,
                bytes);
    te->payload_off = off_of(m);
    te->payload_len = bytes;
    te->type        = src.type;
    te->cardinality = src.cardinality;
    root()->cardinality += src.cardinality;
}

// ============================================================
// Point operations
// ============================================================

bool Bitmap::Contains(uint32_t value) const noexcept {
    const uint16_t hi = static_cast<uint16_t>(value >> 16);
    const uint16_t lo = static_cast<uint16_t>(value & 0xFFFFu);
    const auto* e = find_chunk(hi);
    if (!e) return false;
    if (e->type == 1)
        return ((bmp(*e)[lo >> 6] >> (lo & 63u)) & 1u) != 0u;
    const uint16_t* a = arr(*e);
    uint32_t l = 0, r = e->cardinality;
    while (l < r) {
        uint32_t mid = (l + r) >> 1;
        if (a[mid] == lo) return true;
        if (a[mid] < lo) l = mid + 1;
        else             r = mid;
    }
    return false;
}

uint64_t Bitmap::Cardinality() const noexcept { return root()->cardinality; }
bool     Bitmap::IsEmpty()     const noexcept { return root()->cardinality == 0; }

void Bitmap::Add(uint32_t value) {
    const uint16_t hi = static_cast<uint16_t>(value >> 16);
    const uint16_t lo = static_cast<uint16_t>(value & 0xFFFFu);
    auto* e = get_or_create(hi);
    if (e->type == 1) {
        uint64_t& w    = bmp(*e)[lo >> 6];
        uint64_t  mask = uint64_t(1) << (lo & 63u);
        if (w & mask) return;
        w |= mask;
        e->cardinality++;
        root()->cardinality++;
    } else {
        uint16_t* a    = arr(*e);
        uint32_t  n    = e->cardinality;
        uint32_t  pos  = static_cast<uint32_t>(std::lower_bound(a, a + n, lo) - a);
        if (pos < n && a[pos] == lo) return;
        uint32_t cur_cap = e->payload_len / static_cast<uint32_t>(sizeof(uint16_t));
        if (n >= cur_cap) {
            grow_array(*e, std::max(cur_cap * 2, kArrayInitCap));
            a = arr(*e);
        }
        std::memmove(a + pos + 1, a + pos, (n - pos) * sizeof(uint16_t));
        a[pos] = lo;
        e->cardinality++;
        root()->cardinality++;
        if (e->cardinality >= kArrayToBitmapThreshold) to_bitmap(*e);
    }
    alloc_->PublishFence();
}

void Bitmap::Remove(uint32_t value) {
    const uint16_t hi = static_cast<uint16_t>(value >> 16);
    const uint16_t lo = static_cast<uint16_t>(value & 0xFFFFu);
    auto* r = root();
    if (r->chunk_count == 0) return;
    uint32_t pos = chunk_pos(hi);
    if (pos >= r->chunk_count || idx()[pos].hi != hi) return;
    auto& e = idx()[pos];
    if (e.type == 1) {
        uint64_t& w    = bmp(e)[lo >> 6];
        uint64_t  mask = uint64_t(1) << (lo & 63u);
        if (!(w & mask)) return;
        w &= ~mask;
        e.cardinality--;
        r->cardinality--;
        if (e.cardinality < kBitmapToArrayThreshold) to_array(e);
    } else {
        uint16_t* a   = arr(e);
        uint32_t  n   = e.cardinality;
        uint32_t  epos = static_cast<uint32_t>(std::lower_bound(a, a + n, lo) - a);
        if (epos >= n || a[epos] != lo) return;
        std::memmove(a + epos, a + epos + 1, (n - epos - 1) * sizeof(uint16_t));
        e.cardinality--;
        r->cardinality--;
    }
    if (e.cardinality == 0) erase_chunk(pos);
    alloc_->PublishFence();
}

// ============================================================
// BulkAdd
// ============================================================

void Bitmap::BulkAdd(const uint32_t* sorted_values, size_t count) {
    if (!count) return;
    size_t i = 0;
    while (i < count) {
        const uint16_t hi    = static_cast<uint16_t>(sorted_values[i] >> 16);
        const size_t   start = i;
        while (i < count && static_cast<uint16_t>(sorted_values[i] >> 16) == hi) ++i;
        // sorted_values[start..i) share the same hi.

        auto* e = get_or_create(hi);

        if (e->type == 1) {
            uint64_t* b = bmp(*e);
            for (size_t k = start; k < i; ++k) {
                uint16_t lo   = static_cast<uint16_t>(sorted_values[k] & 0xFFFFu);
                uint64_t& w   = b[lo >> 6];
                uint64_t  mask = uint64_t(1) << (lo & 63u);
                if (!(w & mask)) { w |= mask; e->cardinality++; root()->cardinality++; }
            }
        } else if (e->cardinality == 0) {
            // Empty array: fill directly with sorted lo values.
            uint32_t n       = static_cast<uint32_t>(i - start);
            uint32_t cur_cap = e->payload_len / static_cast<uint32_t>(sizeof(uint16_t));
            if (cur_cap < n) grow_array(*e, std::max(n, kArrayInitCap));
            uint16_t* a = arr(*e);
            uint32_t  k = 0;
            uint16_t  prev = 0;
            bool first = true;
            for (size_t s = start; s < i; ++s) {
                uint16_t lo = static_cast<uint16_t>(sorted_values[s] & 0xFFFFu);
                if (first || lo != prev) { a[k++] = lo; prev = lo; first = false; }
            }
            e->cardinality = k;
            root()->cardinality += k;
        } else {
            // Merge existing sorted array with new sorted lo values.
            const uint16_t* old_a   = arr(*e);
            uint32_t        old_n   = e->cardinality;
            uint32_t        new_n   = static_cast<uint32_t>(i - start);
            uint32_t        merged_cap = old_n + new_n;
            void*           merged_mem = alloc_->Malloc(merged_cap * sizeof(uint16_t));
            auto*           merged     = static_cast<uint16_t*>(merged_mem);
            uint32_t ai = 0, ni = 0, out = 0;
            while (ai < old_n && ni < new_n) {
                uint16_t av = old_a[ai];
                uint16_t nv = static_cast<uint16_t>(sorted_values[start + ni] & 0xFFFFu);
                if (av < nv)      { merged[out++] = av; ++ai; }
                else if (av > nv) { merged[out++] = nv; ++ni; }
                else              { merged[out++] = av; ++ai; ++ni; }  // dedup
            }
            while (ai < old_n) merged[out++] = old_a[ai++];
            while (ni < new_n) {
                uint16_t nv = static_cast<uint16_t>(sorted_values[start + ni] & 0xFFFFu);
                if (out == 0 || merged[out - 1] != nv) merged[out++] = nv;
                ++ni;
            }
            uint64_t old_card = e->cardinality;
            alloc_->Free(alloc_->OffsetToPtr(e->payload_off), FreeMode::Delayed);
            e->payload_off = off_of(merged_mem);
            e->payload_len = merged_cap * static_cast<uint32_t>(sizeof(uint16_t));
            e->cardinality = out;
            root()->cardinality += (out - old_card);
        }
        if (e->type == 0 && e->cardinality >= kArrayToBitmapThreshold)
            to_bitmap(*e);
    }
    alloc_->PublishFence();
}

// ============================================================
// OrWith
// ============================================================

void Bitmap::OrWith(const Bitmap& o) {
    if (this == &o) return;
    const auto* or_  = o.root();
    const auto* oidx = (or_->chunks_off == 0) ? nullptr : o.idx();
    for (uint32_t ci = 0; ci < or_->chunk_count; ++ci) {
        const auto& oe  = oidx[ci];
        auto*       te  = get_or_create(oe.hi);
        if (te->cardinality == 0) {
            copy_append_chunk(oe.hi, oe, o);
            continue;
        }
        const uint32_t old_card = te->cardinality;
        if (te->type == 0 && oe.type == 0) {
            const uint16_t* ta = arr(*te);
            const uint16_t* oa = o.arr(oe);
            uint32_t tn = te->cardinality, on = oe.cardinality;
            void*    m  = alloc_->Malloc((tn + on) * sizeof(uint16_t));
            auto*    mg = static_cast<uint16_t*>(m);
            uint32_t ai = 0, bi = 0, out = 0;
            while (ai < tn && bi < on) {
                if      (ta[ai] < oa[bi]) mg[out++] = ta[ai++];
                else if (ta[ai] > oa[bi]) mg[out++] = oa[bi++];
                else { mg[out++] = ta[ai++]; ++bi; }
            }
            while (ai < tn) mg[out++] = ta[ai++];
            while (bi < on) mg[out++] = oa[bi++];
            alloc_->Free(alloc_->OffsetToPtr(te->payload_off), FreeMode::Delayed);
            te->payload_off = off_of(m);
            te->payload_len = (tn + on) * static_cast<uint32_t>(sizeof(uint16_t));
            te->cardinality = out;
            root()->cardinality += (out - old_card);
            if (te->cardinality >= kArrayToBitmapThreshold) to_bitmap(*te);
        } else {
            uint64_t tw[kBitmapWords], ow[kBitmapWords];
            chunk_to_words(*te, tw);
            o.chunk_to_words(oe, ow);
            for (uint32_t k = 0; k < kBitmapWords; ++k) tw[k] |= ow[k];
            uint32_t new_card = popcount_words(tw);
            root()->cardinality += (new_card - old_card);
            build_from_words(*te, tw, new_card);
        }
    }
    alloc_->PublishFence();
}

// ============================================================
// AndWith
// ============================================================

void Bitmap::AndWith(const Bitmap& o) {
    if (this == &o) return;
    auto* r  = root();
    uint32_t ci = 0;
    while (ci < r->chunk_count) {
        const uint16_t  hi  = idx()[ci].hi;
        const auto*     oe  = o.find_chunk(hi);
        if (!oe) {
            r->cardinality -= idx()[ci].cardinality;
            erase_chunk(ci);
            continue;
        }
        auto& te = idx()[ci];
        const uint32_t old_card = te.cardinality;
        if (te.type == 0 && oe->type == 0) {
            const uint16_t* ta = arr(te);
            const uint16_t* oa = o.arr(*oe);
            uint32_t tn = te.cardinality, on = oe->cardinality;
            void*    m  = alloc_->Malloc(std::min(tn, on) * sizeof(uint16_t));
            auto*    is = static_cast<uint16_t*>(m);
            uint32_t ai = 0, bi = 0, out = 0;
            while (ai < tn && bi < on) {
                if (ta[ai] == oa[bi]) { is[out++] = ta[ai++]; ++bi; }
                else if (ta[ai] < oa[bi]) ++ai;
                else ++bi;
            }
            alloc_->Free(alloc_->OffsetToPtr(te.payload_off), FreeMode::Delayed);
            te.payload_off = off_of(m);
            te.payload_len = std::min(tn, on) * static_cast<uint32_t>(sizeof(uint16_t));
            te.cardinality = out;
            r->cardinality -= (old_card - out);
            if (out == 0) { erase_chunk(ci); continue; }
        } else {
            uint64_t tw[kBitmapWords], ow[kBitmapWords];
            chunk_to_words(te, tw);
            o.chunk_to_words(*oe, ow);
            for (uint32_t k = 0; k < kBitmapWords; ++k) tw[k] &= ow[k];
            uint32_t new_card = popcount_words(tw);
            r->cardinality -= (old_card - new_card);
            if (new_card == 0) { erase_chunk(ci); continue; }
            build_from_words(te, tw, new_card);
        }
        ++ci;
    }
    alloc_->PublishFence();
}

// ============================================================
// XorWith (private) / AndNotWith (private)
// ============================================================

void Bitmap::xor_with(const Bitmap& o) {
    if (this == &o) {
        // A XOR A = empty.
        auto* r = root();
        for (uint32_t ci = 0; ci < r->chunk_count; ++ci) {
            auto& e = idx()[ci];
            if (e.payload_off)
                alloc_->Free(alloc_->OffsetToPtr(e.payload_off), FreeMode::Delayed);
        }
        if (r->chunks_off)
            alloc_->Free(alloc_->OffsetToPtr(r->chunks_off), FreeMode::Delayed);
        r->cardinality    = 0;
        r->chunk_count    = 0;
        r->chunk_capacity = 0;
        r->chunks_off     = 0;
        alloc_->PublishFence();
        return;
    }

    // Snapshot the original hi-keys of *this before any modification.
    // We need this to distinguish, in the second pass, between:
    //   (a) chunks in o but never in this → should be copied in
    //   (b) chunks in o that were in this but XOR'd to empty → must NOT be re-copied
    const auto* r0 = root();
    const uint32_t orig_n = r0->chunk_count;
    // Use a small stack buffer for the common case; fall back to arena alloc.
    uint16_t  stack_buf[128];
    uint16_t* orig_his = (orig_n <= 128)
                             ? stack_buf
                             : static_cast<uint16_t*>(
                                   alloc_->Malloc(orig_n * sizeof(uint16_t)));
    for (uint32_t i = 0; i < orig_n; ++i)
        orig_his[i] = idx()[i].hi;  // already sorted ascending

    // Pass 1: XOR every chunk that exists in both original this AND o.
    auto* r  = root();
    uint32_t ci = 0;
    while (ci < r->chunk_count) {
        const uint16_t hi = idx()[ci].hi;
        const auto*    oe = o.find_chunk(hi);
        if (!oe) { ++ci; continue; }
        auto& te = idx()[ci];
        const uint32_t old_card = te.cardinality;
        uint64_t tw[kBitmapWords], ow[kBitmapWords];
        chunk_to_words(te, tw);
        o.chunk_to_words(*oe, ow);
        for (uint32_t k = 0; k < kBitmapWords; ++k) tw[k] ^= ow[k];
        uint32_t new_card = popcount_words(tw);
        r->cardinality += new_card;
        r->cardinality -= old_card;
        if (new_card == 0) { erase_chunk(ci); continue; }
        build_from_words(te, tw, new_card);
        ++ci;
    }

    // Pass 2: copy chunks from o that were NOT in the original this.
    const auto* oidx = (o.root()->chunks_off == 0) ? nullptr : o.idx();
    for (uint32_t cj = 0; cj < o.root()->chunk_count; ++cj) {
        const auto& oe = oidx[cj];
        // Binary search in the snapshot (orig_his is sorted).
        bool in_orig = std::binary_search(orig_his, orig_his + orig_n, oe.hi);
        if (!in_orig)
            copy_append_chunk(oe.hi, oe, o);
    }

    if (orig_n > 128)
        alloc_->Free(orig_his, FreeMode::Delayed);
    alloc_->PublishFence();
}

void Bitmap::and_not_with(const Bitmap& o) {
    if (this == &o) {
        // A ANDNOT A = empty
        auto* r = root();
        for (uint32_t ci = 0; ci < r->chunk_count; ++ci) {
            auto& e = idx()[ci];
            if (e.payload_off)
                alloc_->Free(alloc_->OffsetToPtr(e.payload_off), FreeMode::Delayed);
        }
        if (r->chunks_off)
            alloc_->Free(alloc_->OffsetToPtr(r->chunks_off), FreeMode::Delayed);
        r->cardinality    = 0;
        r->chunk_count    = 0;
        r->chunk_capacity = 0;
        r->chunks_off     = 0;
        alloc_->PublishFence();
        return;
    }
    auto* r  = root();
    uint32_t ci = 0;
    while (ci < r->chunk_count) {
        const uint16_t hi  = idx()[ci].hi;
        const auto*    oe  = o.find_chunk(hi);
        if (!oe) { ++ci; continue; }
        auto& te = idx()[ci];
        const uint32_t old_card = te.cardinality;
        if (te.type == 0 && oe->type == 0) {
            const uint16_t* ta = arr(te);
            const uint16_t* oa = o.arr(*oe);
            uint32_t tn = te.cardinality, on = oe->cardinality;
            void*    m  = alloc_->Malloc(tn * sizeof(uint16_t));
            auto*    df = static_cast<uint16_t*>(m);
            uint32_t ai = 0, bi = 0, out = 0;
            while (ai < tn && bi < on) {
                if (ta[ai] < oa[bi])      df[out++] = ta[ai++];
                else if (ta[ai] > oa[bi]) ++bi;
                else { ++ai; ++bi; }
            }
            while (ai < tn) df[out++] = ta[ai++];
            alloc_->Free(alloc_->OffsetToPtr(te.payload_off), FreeMode::Delayed);
            te.payload_off = off_of(m);
            te.payload_len = tn * static_cast<uint32_t>(sizeof(uint16_t));
            te.cardinality = out;
            r->cardinality -= (old_card - out);
            if (out == 0) { erase_chunk(ci); continue; }
        } else {
            uint64_t tw[kBitmapWords], ow[kBitmapWords];
            chunk_to_words(te, tw);
            o.chunk_to_words(*oe, ow);
            for (uint32_t k = 0; k < kBitmapWords; ++k) tw[k] &= ~ow[k];
            uint32_t new_card = popcount_words(tw);
            r->cardinality -= (old_card - new_card);
            if (new_card == 0) { erase_chunk(ci); continue; }
            build_from_words(te, tw, new_card);
        }
        ++ci;
    }
    alloc_->PublishFence();
}

// ============================================================
// Value-returning set algebra
// ============================================================

Bitmap Bitmap::Or    (const Bitmap& o) const { Bitmap r(alloc_, 0); r.OrWith(*this); r.OrWith(o); return r; }
Bitmap Bitmap::And   (const Bitmap& o) const { Bitmap r(alloc_, 0); r.OrWith(*this); r.AndWith(o); return r; }
Bitmap Bitmap::AndNot(const Bitmap& o) const { Bitmap r(alloc_, 0); r.OrWith(*this); r.and_not_with(o); return r; }
Bitmap Bitmap::Xor   (const Bitmap& o) const { Bitmap r(alloc_, 0); r.OrWith(*this); r.xor_with(o); return r; }

// ============================================================
// EstimatedBytes
// ============================================================

size_t Bitmap::EstimatedBytes() const noexcept {
    const auto* r = root();
    size_t total = sizeof(Bitmap) + sizeof(bm_detail::BmRoot);
    total += static_cast<size_t>(r->chunk_capacity) * sizeof(bm_detail::BmChunkEntry);
    const auto* entries = (r->chunks_off == 0) ? nullptr : idx();
    for (uint32_t ci = 0; ci < r->chunk_count; ++ci)
        total += entries[ci].payload_len;
    return total;
}

// ============================================================
// Serialize / Deserialize
// ============================================================

static void write_u16(std::vector<uint8_t>& out, uint16_t v) {
    size_t pos = out.size();
    out.resize(pos + 2);
    std::memcpy(out.data() + pos, &v, 2);
}
static void write_u32(std::vector<uint8_t>& out, uint32_t v) {
    size_t pos = out.size();
    out.resize(pos + 4);
    std::memcpy(out.data() + pos, &v, 4);
}
static uint16_t read_u16(const uint8_t* p) noexcept {
    uint16_t v; std::memcpy(&v, p, 2); return v;
}
static uint32_t read_u32(const uint8_t* p) noexcept {
    uint32_t v; std::memcpy(&v, p, 4); return v;
}

std::vector<uint8_t> Bitmap::Serialize() const {
    const auto* r = root();
    std::vector<uint8_t> out;
    out.reserve(16 + r->chunk_count * 32);
    write_u32(out, bm_detail::kWireMagic);
    write_u16(out, bm_detail::kWireVersion);
    write_u16(out, 0);  // reserved
    write_u32(out, r->chunk_count);
    const auto* entries = (r->chunks_off == 0) ? nullptr : idx();
    for (uint32_t ci = 0; ci < r->chunk_count; ++ci) {
        const auto& e = entries[ci];
        uint32_t payload_bytes = (e.type == 1)
            ? kBitmapWords * static_cast<uint32_t>(sizeof(uint64_t))
            : e.cardinality * static_cast<uint32_t>(sizeof(uint16_t));
        write_u16(out, e.hi);
        out.push_back(e.type);
        out.push_back(0);
        write_u32(out, e.cardinality);
        write_u32(out, payload_bytes);
        size_t pos = out.size();
        out.resize(pos + payload_bytes);
        std::memcpy(out.data() + pos,
                    static_cast<const char*>(base_) + e.payload_off,
                    payload_bytes);
    }
    return out;
}

Bitmap Bitmap::Deserialize(Allocator* alloc, const uint8_t* data, size_t size) {
    if (!data)      throw std::runtime_error("Bitmap::Deserialize: null data");
    if (size < 12)  throw std::runtime_error("Bitmap::Deserialize: payload too small");

    std::unique_ptr<yikv::alloc::FtAllocator> owned;
    if (!alloc) {
        alloc::AllocatorOptions opts;
        opts.arena_size = 16ull * 1024 * 1024;
        owned = std::make_unique<yikv::alloc::FtAllocator>(opts);
        alloc = owned.get();
    }

    size_t pos = 0;
    uint32_t magic   = read_u32(data + pos); pos += 4;
    uint16_t version = read_u16(data + pos); pos += 2;
    pos += 2;  // reserved
    uint32_t nchunks = read_u32(data + pos); pos += 4;

    if (magic   != bm_detail::kWireMagic)   throw std::runtime_error("Bitmap::Deserialize: bad magic");
    if (version != bm_detail::kWireVersion)  throw std::runtime_error("Bitmap::Deserialize: unsupported version");

    Bitmap bm(alloc, 0);
    bm.owned_alloc_ = std::move(owned);

    uint16_t prev_hi = 0;
    bool     has_prev = false;
    for (uint32_t i = 0; i < nchunks; ++i) {
        if (pos + 12 > size)
            throw std::runtime_error("Bitmap::Deserialize: truncated chunk header");
        uint16_t hi        = read_u16(data + pos); pos += 2;
        uint8_t  type      = data[pos++];
        pos++;  // reserved
        uint32_t card      = read_u32(data + pos); pos += 4;
        uint32_t pay_bytes = read_u32(data + pos); pos += 4;
        if (has_prev && hi <= prev_hi)
            throw std::runtime_error("Bitmap::Deserialize: unsorted chunks");
        has_prev = true; prev_hi = hi;
        if (type > 1)
            throw std::runtime_error("Bitmap::Deserialize: invalid chunk type");
        if (pos + pay_bytes > size)
            throw std::runtime_error("Bitmap::Deserialize: truncated chunk payload");

        if (type == 0) {
            // Array chunk.
            if (pay_bytes != card * 2u)
                throw std::runtime_error("Bitmap::Deserialize: array payload size mismatch");
            std::vector<uint32_t> vals(card);
            const uint16_t* src = reinterpret_cast<const uint16_t*>(data + pos);
            for (uint32_t j = 1; j < card; ++j)
                if (src[j] <= src[j - 1])
                    throw std::runtime_error("Bitmap::Deserialize: array not sorted");
            for (uint32_t j = 0; j < card; ++j)
                vals[j] = (static_cast<uint32_t>(hi) << 16) | src[j];
            bm.BulkAdd(vals.data(), vals.size());
        } else {
            // Bitmap chunk.
            if (pay_bytes != kBitmapWords * 8u)
                throw std::runtime_error("Bitmap::Deserialize: bitmap payload size mismatch");
            const uint64_t* words = reinterpret_cast<const uint64_t*>(data + pos);
            uint32_t pop = 0;
            for (uint32_t w = 0; w < kBitmapWords; ++w)
                pop += static_cast<uint32_t>(__builtin_popcountll(words[w]));
            if (pop != card)
                throw std::runtime_error("Bitmap::Deserialize: cardinality mismatch");
            // Reconstruct via BulkAdd.
            std::vector<uint32_t> vals;
            vals.reserve(card);
            for (uint32_t w = 0; w < kBitmapWords; ++w) {
                uint64_t word = words[w];
                while (word) {
                    uint32_t bit = static_cast<uint32_t>(__builtin_ctzll(word));
                    vals.push_back((static_cast<uint32_t>(hi) << 16) | (w << 6) | bit);
                    word &= word - 1;
                }
            }
            bm.BulkAdd(vals.data(), vals.size());
        }
        pos += pay_bytes;
    }
    if (pos != size)
        throw std::runtime_error("Bitmap::Deserialize: trailing bytes");
    return bm;
}

Bitmap Bitmap::Deserialize(const uint8_t* data, size_t size) {
    return Deserialize(nullptr, data, size);
}

}  // namespace container
}  // namespace yikv
