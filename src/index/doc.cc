#include "src/index/doc.h"

#include <cstring>

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

int32_t Doc::array_get_int32(uint32_t fid, uint32_t i) const {
    return array_data<int32_t>(fid)[i];
}
void Doc::array_put_int32(uint32_t fid, const int32_t* data, uint32_t count) {
    array_put_impl<int32_t>(fid, data, count);
}
void Doc::array_append_int32(uint32_t fid, int32_t val) {
    array_append_impl<int32_t>(fid, val);
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

float Doc::array_get_float(uint32_t fid, uint32_t i) const {
    return array_data<float>(fid)[i];
}
void Doc::array_put_float(uint32_t fid, const float* data, uint32_t count) {
    array_put_impl<float>(fid, data, count);
}

double Doc::array_get_double(uint32_t fid, uint32_t i) const {
    return array_data<double>(fid)[i];
}
void Doc::array_put_double(uint32_t fid, const double* data, uint32_t count) {
    array_put_impl<double>(fid, data, count);
}

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
    size_t sum = 0;
    for (uint32_t i = 0; i < count; ++i) sum += parts[i].size();

    uint64_t cap = static_cast<uint64_t>(count) * 12 / 10;
    if (cap < count) cap = count;
    if (cap == 0) cap = 1;

    const size_t header = sizeof(uint64_t) + sizeof(uint32_t) + sizeof(uint32_t) * static_cast<size_t>(count);
    const size_t bytes  = header + sum;
    void*        mem    = alloc_->Malloc(bytes);
    std::memset(mem, 0, bytes);
    std::memcpy(mem, &cap, sizeof(uint64_t));
    size_t               off  = sizeof(uint64_t);
    std::memcpy(reinterpret_cast<char*>(mem) + off, &count, sizeof(uint32_t));
    off += sizeof(uint32_t);
    uint32_t* lens = reinterpret_cast<uint32_t*>(reinterpret_cast<char*>(mem) + off);
    off += sizeof(uint32_t) * static_cast<size_t>(count);
    char* payload = reinterpret_cast<char*>(mem) + off;
    size_t p = 0;
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
    uint32_t n = array_size(fid);
    if (i >= n || n == 0) return {};
    uint64_t blob_off = slot(fid).b;
    if (blob_off == 0) return {};
    const char* base = reinterpret_cast<const char*>(alloc_->OffsetToPtr(blob_off));
    uint32_t    count;
    std::memcpy(&count, base + sizeof(uint64_t), sizeof(uint32_t));
    if (i >= count) return {};
    const uint32_t* lens =
        reinterpret_cast<const uint32_t*>(base + sizeof(uint64_t) + sizeof(uint32_t));
    const size_t payload_off =
        sizeof(uint64_t) + sizeof(uint32_t) + sizeof(uint32_t) * static_cast<size_t>(count);
    size_t pos = 0;
    for (uint32_t j = 0; j < i; ++j) pos += lens[j];
    return std::string_view(base + payload_off + pos, lens[i]);
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

}  // namespace index
}  // namespace yikv
