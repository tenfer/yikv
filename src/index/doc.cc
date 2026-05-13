#include "src/index/doc.h"

#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace yikv {
namespace index {

Doc::Doc(alloc::Allocator* alloc, uint32_t n_slots, uint32_t doc_id)
    : alloc_(alloc) {
    size_t bytes = sizeof(DocHeader) + static_cast<size_t>(n_slots) * sizeof(Slot);
    void*  mem   = alloc->Malloc(bytes);
    std::memset(mem, 0, bytes);
    off_ = alloc->PtrToOffset(mem);
    DocHeader* hdr = reinterpret_cast<DocHeader*>(mem);
    hdr->doc_id  = doc_id;
    hdr->n_slots = n_slots;
}

Doc::Doc(alloc::Allocator* alloc, uint64_t off)
    : alloc_(alloc), off_(off) {}

Doc::Slot* Doc::slots() const {
    char* p = reinterpret_cast<char*>(alloc_->OffsetToPtr(off_));
    return reinterpret_cast<Slot*>(p + sizeof(DocHeader));
}

Doc::Slot& Doc::slot(uint32_t fid) const {
    return slots()[fid];
}

uint32_t Doc::doc_id() const noexcept {
    return reinterpret_cast<const DocHeader*>(alloc_->OffsetToPtr(off_))->doc_id;
}

int32_t Doc::get_int32(uint32_t fid) const {
    int32_t v;
    uint32_t lo = static_cast<uint32_t>(slot(fid).a);
    std::memcpy(&v, &lo, sizeof(v));
    return v;
}

void Doc::put_int32(uint32_t fid, int32_t v) {
    uint32_t lo;
    std::memcpy(&lo, &v, sizeof(lo));
    slot(fid).a = lo;
    slot(fid).b = 0;
}

int64_t Doc::get_int64(uint32_t fid) const {
    int64_t v;
    uint64_t raw = slot(fid).a;
    std::memcpy(&v, &raw, sizeof(v));
    return v;
}

void Doc::put_int64(uint32_t fid, int64_t v) {
    uint64_t raw;
    std::memcpy(&raw, &v, sizeof(raw));
    slot(fid).a = raw;
    slot(fid).b = 0;
}

float Doc::get_float(uint32_t fid) const {
    float v;
    uint32_t lo = static_cast<uint32_t>(slot(fid).a);
    std::memcpy(&v, &lo, sizeof(v));
    return v;
}

void Doc::put_float(uint32_t fid, float v) {
    uint32_t lo;
    std::memcpy(&lo, &v, sizeof(lo));
    slot(fid).a = lo;
    slot(fid).b = 0;
}

double Doc::get_double(uint32_t fid) const {
    double v;
    uint64_t raw = slot(fid).a;
    std::memcpy(&v, &raw, sizeof(v));
    return v;
}

void Doc::put_double(uint32_t fid, double v) {
    uint64_t raw;
    std::memcpy(&raw, &v, sizeof(raw));
    slot(fid).a = raw;
    slot(fid).b = 0;
}

std::string_view Doc::get_string(uint32_t fid) const {
    const Slot& s = slot(fid);
    if (s.b == 0) return {};
    const char* data = reinterpret_cast<const char*>(alloc_->OffsetToPtr(s.b));
    return std::string_view(data, static_cast<size_t>(s.a));
}

void Doc::Retire() {
    if (!valid()) return;
    const DocHeader* hdr =
        reinterpret_cast<const DocHeader*>(alloc_->OffsetToPtr(off_));
    uint32_t n  = hdr->n_slots;
    Slot*    ss = slots();
    // Free every variable-length payload (strings and arrays have slot.b != 0;
    // fixed scalars always leave slot.b == 0).
    for (uint32_t i = 0; i < n; ++i) {
        if (ss[i].b != 0) {
            alloc_->Free(alloc_->OffsetToPtr(ss[i].b),
                         alloc::FreeMode::Delayed);
        }
    }
    // Free the root block (DocHeader + Slot[]).
    alloc_->Free(alloc_->OffsetToPtr(off_), alloc::FreeMode::Delayed);
}

void Doc::put_string(uint32_t fid, std::string_view v) {
    Slot& s = slot(fid);
    if (s.b != 0) {
        // Free the previous string payload before allocating a new one.
        alloc_->Free(alloc_->OffsetToPtr(s.b), alloc::FreeMode::Delayed);
    }
    size_t len = v.size();
    void*  mem = alloc_->Malloc(len > 0 ? len : 1);
    if (len > 0) std::memcpy(mem, v.data(), len);
    s.a = static_cast<uint64_t>(len);
    s.b = alloc_->PtrToOffset(mem);
}

uint32_t Doc::array_size(uint32_t fid) const {
    return static_cast<uint32_t>(slot(fid).a);
}

std::pair<const int32_t*, uint32_t> Doc::array_view_int32(uint32_t fid) const {
    uint32_t n = array_size(fid);
    if (n == 0) return {nullptr, 0};
    return {array_data<int32_t>(fid), n};
}

std::pair<const int64_t*, uint32_t> Doc::array_view_int64(uint32_t fid) const {
    uint32_t n = array_size(fid);
    if (n == 0) return {nullptr, 0};
    return {array_data<int64_t>(fid), n};
}

std::pair<const float*, uint32_t> Doc::array_view_float(uint32_t fid) const {
    uint32_t n = array_size(fid);
    if (n == 0) return {nullptr, 0};
    return {array_data<float>(fid), n};
}

std::pair<const double*, uint32_t> Doc::array_view_double(uint32_t fid) const {
    uint32_t n = array_size(fid);
    if (n == 0) return {nullptr, 0};
    return {array_data<double>(fid), n};
}

uint64_t Doc::array_capacity(uint32_t fid) const {
    uint64_t off = slot(fid).b;
    if (off == 0) return 0;
    uint64_t cap;
    std::memcpy(&cap, alloc_->OffsetToPtr(off), sizeof(uint64_t));
    return cap;
}

template <typename T>
T* Doc::array_data(uint32_t fid) const {
    uint64_t off = slot(fid).b;
    if (off == 0) return nullptr;
    char* base = reinterpret_cast<char*>(alloc_->OffsetToPtr(off));
    return reinterpret_cast<T*>(base + sizeof(uint64_t));
}

template <typename T>
void Doc::array_put_impl(uint32_t fid, const T* data, uint32_t count) {
    Slot& s = slot(fid);
    if (s.b != 0) {
        // Free the previous array buffer before allocating a new one.
        alloc_->Free(alloc_->OffsetToPtr(s.b), alloc::FreeMode::Delayed);
    }

    uint64_t cap = static_cast<uint64_t>(count) * 12 / 10;
    if (cap < count) cap = count;
    if (cap == 0) cap = 1;

    size_t bytes = sizeof(uint64_t) + cap * sizeof(T);
    void*  mem   = alloc_->Malloc(bytes);
    std::memset(mem, 0, bytes);
    std::memcpy(mem, &cap, sizeof(uint64_t));

    T* elems = reinterpret_cast<T*>(reinterpret_cast<char*>(mem) + sizeof(uint64_t));
    if (data && count > 0) std::memcpy(elems, data, count * sizeof(T));

    s.a = static_cast<uint64_t>(count);
    s.b = alloc_->PtrToOffset(mem);
}

template <typename T>
void Doc::array_append_impl(uint32_t fid, T val) {
    uint64_t count = slot(fid).a;
    uint64_t cap   = array_capacity(fid);

    if (count < cap) {
        array_data<T>(fid)[count] = val;
        slot(fid).a = count + 1;
        return;
    }

    uint64_t new_cap = (count + 1) * 12 / 10;
    if (new_cap <= count) new_cap = count + 1;

    size_t bytes = sizeof(uint64_t) + new_cap * sizeof(T);
    void*  mem   = alloc_->Malloc(bytes);
    std::memset(mem, 0, bytes);
    std::memcpy(mem, &new_cap, sizeof(uint64_t));

    T* new_elems = reinterpret_cast<T*>(reinterpret_cast<char*>(mem) + sizeof(uint64_t));
    if (count > 0) {
        std::memcpy(new_elems, array_data<T>(fid), count * sizeof(T));
    }
    new_elems[count] = val;

    // Free the old (now-replaced) buffer.
    uint64_t old_off = slot(fid).b;
    if (old_off != 0) {
        alloc_->Free(alloc_->OffsetToPtr(old_off), alloc::FreeMode::Delayed);
    }
    slot(fid).a = count + 1;
    slot(fid).b = alloc_->PtrToOffset(mem);
}

template <typename T>
void Doc::array_append_batch_impl(uint32_t fid, const T* data, uint32_t count) {
    if (!data || count == 0) return;

    Slot& s = slot(fid);
    // Empty slot: delegate to put_impl which already sizes with 1.2x slack.
    if (s.b == 0) {
        array_put_impl<T>(fid, data, count);
        return;
    }

    const uint64_t cur_count  = s.a;
    const uint64_t cap        = array_capacity(fid);
    const uint64_t need_count = cur_count + count;

    // Fast path: capacity already covers the batch, write in place.
    if (need_count <= cap) {
        T* elems = array_data<T>(fid);
        std::memcpy(elems + cur_count, data,
                    static_cast<size_t>(count) * sizeof(T));
        s.a = need_count;
        return;
    }

    // Slow path: grow geometrically to at least need_count (1.2x).
    uint64_t new_cap = need_count + need_count / 5;
    if (new_cap < need_count) new_cap = need_count;

    const size_t bytes = sizeof(uint64_t) + static_cast<size_t>(new_cap) * sizeof(T);
    void*        mem   = alloc_->Malloc(bytes);
    std::memset(mem, 0, bytes);
    std::memcpy(mem, &new_cap, sizeof(uint64_t));

    T* new_elems = reinterpret_cast<T*>(reinterpret_cast<char*>(mem) + sizeof(uint64_t));
    if (cur_count > 0) {
        std::memcpy(new_elems, array_data<T>(fid),
                    static_cast<size_t>(cur_count) * sizeof(T));
    }
    // Both old and new reads finish before Free below, so `data` may safely
    // alias arena memory unrelated to this slot. It must NOT alias the slot
    // itself (documented in doc.h).
    std::memcpy(new_elems + cur_count, data,
                static_cast<size_t>(count) * sizeof(T));

    const uint64_t old_off = s.b;
    if (old_off != 0) {
        alloc_->Free(alloc_->OffsetToPtr(old_off), alloc::FreeMode::Delayed);
    }
    s.a = need_count;
    s.b = alloc_->PtrToOffset(mem);
}

int32_t Doc::array_get_int32(uint32_t fid, uint32_t i) const {
    return array_data<int32_t>(fid)[i];
}
void Doc::array_put_int32(uint32_t fid, const int32_t* data, uint32_t count) {
    array_put_impl<int32_t>(fid, data, count);
}
void Doc::array_append_int32(uint32_t fid, int32_t val) {
    array_append_impl<int32_t>(fid, val);
}
void Doc::array_append_int32s(uint32_t fid, const int32_t* data, uint32_t count) {
    array_append_batch_impl<int32_t>(fid, data, count);
}

int64_t Doc::array_get_int64(uint32_t fid, uint32_t i) const {
    return array_data<int64_t>(fid)[i];
}
void Doc::array_put_int64(uint32_t fid, const int64_t* data, uint32_t count) {
    array_put_impl<int64_t>(fid, data, count);
}
void Doc::array_append_int64(uint32_t fid, int64_t val) {
    array_append_impl<int64_t>(fid, val);
}
void Doc::array_append_int64s(uint32_t fid, const int64_t* data, uint32_t count) {
    array_append_batch_impl<int64_t>(fid, data, count);
}

float Doc::array_get_float(uint32_t fid, uint32_t i) const {
    return array_data<float>(fid)[i];
}
void Doc::array_put_float(uint32_t fid, const float* data, uint32_t count) {
    array_put_impl<float>(fid, data, count);
}
void Doc::array_append_float(uint32_t fid, float val) {
    array_append_impl<float>(fid, val);
}
void Doc::array_append_floats(uint32_t fid, const float* data, uint32_t count) {
    array_append_batch_impl<float>(fid, data, count);
}

double Doc::array_get_double(uint32_t fid, uint32_t i) const {
    return array_data<double>(fid)[i];
}
void Doc::array_put_double(uint32_t fid, const double* data, uint32_t count) {
    array_put_impl<double>(fid, data, count);
}
void Doc::array_append_double(uint32_t fid, double val) {
    array_append_impl<double>(fid, val);
}
void Doc::array_append_doubles(uint32_t fid, const double* data, uint32_t count) {
    array_append_batch_impl<double>(fid, data, count);
}

namespace {

// String array buffer layout (see doc.h header comment).
//
//   off  0:  uint32  cap_count    capacity of the lens[] array
//   off  4:  uint32  cap_bytes    capacity of the bytes_payload region
//   off  8:  uint32  count        current number of strings (mirrors slot.a)
//   off 12:  uint32  bytes_used   current sum of lens[0..count-1]
//   off 16:  uint32  lens[cap_count]
//   off 16 + 4*cap_count: char bytes_payload[cap_bytes]
constexpr size_t kStrArrHeaderBytes = 16;

inline size_t StrArrPayloadOff(uint32_t cap_count) {
    return kStrArrHeaderBytes + sizeof(uint32_t) * static_cast<size_t>(cap_count);
}

inline size_t StrArrAllocBytes(uint32_t cap_count, uint32_t cap_bytes) {
    return StrArrPayloadOff(cap_count) + static_cast<size_t>(cap_bytes);
}

// Round `need` up to ~1.2x to keep amortized O(1) growth, while clamping at
// uint32_t::max() so the 32-bit on-arena fields can hold the result. `need`
// itself must already fit in uint32_t (the caller checks).
inline uint32_t GrowCap32(uint32_t need) {
    constexpr uint32_t kMax = std::numeric_limits<uint32_t>::max();
    uint64_t grown = static_cast<uint64_t>(need) + need / 5;
    if (grown < need) grown = need;
    if (grown == 0)   grown = 1;
    if (grown > kMax) grown = kMax;
    return static_cast<uint32_t>(grown);
}

inline void StrArrWriteHeader(char* base, uint32_t cap_count, uint32_t cap_bytes,
                              uint32_t count, uint32_t bytes_used) {
    std::memcpy(base +  0, &cap_count,  4);
    std::memcpy(base +  4, &cap_bytes,  4);
    std::memcpy(base +  8, &count,      4);
    std::memcpy(base + 12, &bytes_used, 4);
}

inline void StrArrReadHeader(const char* base, uint32_t* cap_count, uint32_t* cap_bytes,
                             uint32_t* count, uint32_t* bytes_used) {
    std::memcpy(cap_count,  base +  0, 4);
    std::memcpy(cap_bytes,  base +  4, 4);
    std::memcpy(count,      base +  8, 4);
    std::memcpy(bytes_used, base + 12, 4);
}

}  // namespace

void Doc::array_put_string(uint32_t fid, const std::string_view* parts, uint32_t count) {
    Slot& s = slot(fid);
    if (s.b != 0) {
        alloc_->Free(alloc_->OffsetToPtr(s.b), alloc::FreeMode::Delayed);
    }
    if (!parts || count == 0) {
        s.a = 0;
        s.b = 0;
        return;
    }

    uint64_t total_bytes64 = 0;
    for (uint32_t i = 0; i < count; ++i) total_bytes64 += parts[i].size();
    // Defensive: a single put exceeding 4 GiB of string payload is well
    // outside the design envelope; clamp would corrupt data, so refuse via
    // assert in debug and a best-effort truncated allocation in release is
    // out of scope. Documented as a precondition.
    const uint32_t total_bytes = static_cast<uint32_t>(total_bytes64);

    const uint32_t cap_count = GrowCap32(count);
    const uint32_t cap_bytes = GrowCap32(total_bytes);

    const size_t alloc_sz = StrArrAllocBytes(cap_count, cap_bytes);
    void*        mem      = alloc_->Malloc(alloc_sz);
    std::memset(mem, 0, alloc_sz);

    char* base = reinterpret_cast<char*>(mem);
    StrArrWriteHeader(base, cap_count, cap_bytes, count, total_bytes);

    uint32_t* lens    = reinterpret_cast<uint32_t*>(base + kStrArrHeaderBytes);
    char*     payload = base + StrArrPayloadOff(cap_count);
    size_t    p       = 0;
    for (uint32_t i = 0; i < count; ++i) {
        lens[i] = static_cast<uint32_t>(parts[i].size());
        if (!parts[i].empty()) {
            std::memcpy(payload + p, parts[i].data(), parts[i].size());
            p += parts[i].size();
        }
    }
    s.a = count;
    s.b = alloc_->PtrToOffset(mem);
}

std::string_view Doc::array_get_string(uint32_t fid, uint32_t i) const {
    const uint32_t n = array_size(fid);
    if (i >= n) return {};
    const uint64_t blob_off = slot(fid).b;
    if (blob_off == 0) return {};
    const char* base = reinterpret_cast<const char*>(alloc_->OffsetToPtr(blob_off));

    uint32_t cap_count, cap_bytes, count, bytes_used;
    StrArrReadHeader(base, &cap_count, &cap_bytes, &count, &bytes_used);
    if (i >= count) return {};

    const uint32_t* lens =
        reinterpret_cast<const uint32_t*>(base + kStrArrHeaderBytes);
    const size_t payload_off = StrArrPayloadOff(cap_count);
    size_t pos = 0;
    for (uint32_t j = 0; j < i; ++j) pos += lens[j];
    return std::string_view(base + payload_off + pos, lens[i]);
}

void Doc::array_view_string(uint32_t fid,
                            std::vector<std::string_view>* out) const {
    if (!out) return;
    if (array_size(fid) == 0) return;
    const uint64_t blob_off = slot(fid).b;
    if (blob_off == 0) return;
    const char* base = reinterpret_cast<const char*>(alloc_->OffsetToPtr(blob_off));

    uint32_t cap_count, cap_bytes, count, bytes_used;
    StrArrReadHeader(base, &cap_count, &cap_bytes, &count, &bytes_used);
    if (count == 0) return;

    const uint32_t* lens =
        reinterpret_cast<const uint32_t*>(base + kStrArrHeaderBytes);
    const char* payload = base + StrArrPayloadOff(cap_count);

    out->reserve(out->size() + count);
    size_t pos = 0;
    for (uint32_t i = 0; i < count; ++i) {
        out->emplace_back(payload + pos, lens[i]);
        pos += lens[i];
    }
}

std::vector<std::string_view> Doc::array_view_string(uint32_t fid) const {
    std::vector<std::string_view> out;
    array_view_string(fid, &out);
    return out;
}

void Doc::array_append_string(uint32_t fid, std::string_view part) {
    array_append_strings(fid, &part, 1);
}

void Doc::array_append_strings(uint32_t fid, const std::string_view* parts, uint32_t count) {
    if (!parts || count == 0) return;

    Slot& s = slot(fid);
    // Empty slot: fall back to put_string (which sizes the buffer with
    // geometric capacity slack, so subsequent single-element appends stay
    // O(amortized 1)).
    if (s.b == 0) {
        array_put_string(fid, parts, count);
        return;
    }

    char* base = reinterpret_cast<char*>(alloc_->OffsetToPtr(s.b));
    uint32_t cap_count, cap_bytes, cur_count, bytes_used;
    StrArrReadHeader(base, &cap_count, &cap_bytes, &cur_count, &bytes_used);

    uint64_t total_new_bytes = 0;
    for (uint32_t i = 0; i < count; ++i) total_new_bytes += parts[i].size();

    const uint64_t need_count64 = static_cast<uint64_t>(cur_count) + count;
    const uint64_t need_bytes64 = static_cast<uint64_t>(bytes_used) + total_new_bytes;

    // Fast path: append in place when both lens-array and byte-payload have
    // spare capacity for the whole batch.
    if (need_count64 <= cap_count && need_bytes64 <= cap_bytes) {
        uint32_t* lens    = reinterpret_cast<uint32_t*>(base + kStrArrHeaderBytes);
        char*     payload = base + StrArrPayloadOff(cap_count);
        size_t    p       = bytes_used;
        for (uint32_t i = 0; i < count; ++i) {
            lens[cur_count + i] = static_cast<uint32_t>(parts[i].size());
            if (!parts[i].empty()) {
                std::memcpy(payload + p, parts[i].data(), parts[i].size());
                p += parts[i].size();
            }
        }
        const uint32_t new_count      = static_cast<uint32_t>(need_count64);
        const uint32_t new_bytes_used = static_cast<uint32_t>(need_bytes64);
        StrArrWriteHeader(base, cap_count, cap_bytes, new_count, new_bytes_used);
        s.a = new_count;
        return;
    }

    // Slow path: at least one of lens/bytes capacity is short. Allocate a
    // larger buffer (1.2x geometric grow on whichever dimension needs it),
    // copy old payload, append new elements, then free the old buffer.
    // Both old reads and `parts[i].data()` reads complete *before* the
    // Free() at the end, so it is safe for `parts[i]` to alias either the
    // old buffer or unrelated arena memory.
    const uint32_t need_count = static_cast<uint32_t>(need_count64);
    const uint32_t need_bytes = static_cast<uint32_t>(need_bytes64);
    const uint32_t new_cap_count =
        (need_count > cap_count) ? GrowCap32(need_count) : cap_count;
    const uint32_t new_cap_bytes =
        (need_bytes > cap_bytes) ? GrowCap32(need_bytes) : cap_bytes;

    const size_t new_alloc_sz = StrArrAllocBytes(new_cap_count, new_cap_bytes);
    void*        new_mem      = alloc_->Malloc(new_alloc_sz);
    std::memset(new_mem, 0, new_alloc_sz);
    char* new_base = reinterpret_cast<char*>(new_mem);

    StrArrWriteHeader(new_base, new_cap_count, new_cap_bytes, need_count, need_bytes);

    const uint32_t* old_lens =
        reinterpret_cast<const uint32_t*>(base + kStrArrHeaderBytes);
    uint32_t* new_lens = reinterpret_cast<uint32_t*>(new_base + kStrArrHeaderBytes);
    if (cur_count > 0) {
        std::memcpy(new_lens, old_lens, sizeof(uint32_t) * static_cast<size_t>(cur_count));
    }

    const char* old_payload = base     + StrArrPayloadOff(cap_count);
    char*       new_payload = new_base + StrArrPayloadOff(new_cap_count);
    if (bytes_used > 0) {
        std::memcpy(new_payload, old_payload, bytes_used);
    }

    size_t p = bytes_used;
    for (uint32_t i = 0; i < count; ++i) {
        new_lens[cur_count + i] = static_cast<uint32_t>(parts[i].size());
        if (!parts[i].empty()) {
            std::memcpy(new_payload + p, parts[i].data(), parts[i].size());
            p += parts[i].size();
        }
    }

    alloc_->Free(alloc_->OffsetToPtr(s.b), alloc::FreeMode::Delayed);
    s.a = need_count;
    s.b = alloc_->PtrToOffset(new_mem);
}

template int32_t* Doc::array_data<int32_t>(uint32_t) const;
template int64_t* Doc::array_data<int64_t>(uint32_t) const;
template float*   Doc::array_data<float>  (uint32_t) const;
template double*  Doc::array_data<double> (uint32_t) const;

template void Doc::array_put_impl<int32_t>(uint32_t, const int32_t*, uint32_t);
template void Doc::array_put_impl<int64_t>(uint32_t, const int64_t*, uint32_t);
template void Doc::array_put_impl<float>  (uint32_t, const float*,   uint32_t);
template void Doc::array_put_impl<double> (uint32_t, const double*,  uint32_t);

template void Doc::array_append_impl<int32_t>(uint32_t, int32_t);
template void Doc::array_append_impl<int64_t>(uint32_t, int64_t);
template void Doc::array_append_impl<float>  (uint32_t, float);
template void Doc::array_append_impl<double> (uint32_t, double);

template void Doc::array_append_batch_impl<int32_t>(uint32_t, const int32_t*, uint32_t);
template void Doc::array_append_batch_impl<int64_t>(uint32_t, const int64_t*, uint32_t);
template void Doc::array_append_batch_impl<float>  (uint32_t, const float*,   uint32_t);
template void Doc::array_append_batch_impl<double> (uint32_t, const double*,  uint32_t);

}  // namespace index
}  // namespace yikv
