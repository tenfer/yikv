#pragma once

#include <cstdint>
#include <string_view>
#include <utility>

#include "src/alloc/allocator.h"

namespace yikv {
namespace index {

// Arena-native document storage.
//
// Arena layout at off_ (all memory via Allocator):
//
//   [DocHeader: doc_id(4), n_slots(4)]
//   [Slot[0], Slot[1], ..., Slot[n_slots-1]]   each slot = 16 bytes
//
// Slot encoding:
//   Fixed scalar (int32/float)  : a = value as uint32 (low 4 bytes), b = 0
//   Fixed scalar (int64/double) : a = value as uint64,                b = 0
//   String                      : a = byte_len,        b = arena_off -> char data
//   Array                       : a = elem_count,      b = arena_off -> [uint64 cap | elem data...]
//
// Field access uses field_id as the slot index; match getters/setters to the schema type.
//
// Variable-length overwrites use FreeMode::Delayed on the previous payload; recycling
// follows AllocatorOptions::reclaim_delay_ns and ReclaimExpired() (see alloc README).

class Doc {
public:
    Doc() = default;

    Doc(alloc::Allocator* alloc, uint32_t n_slots, uint32_t doc_id);
    Doc(alloc::Allocator* alloc, uint64_t off);

    bool     valid()       const noexcept { return alloc_ != nullptr && off_ != 0; }
    uint32_t doc_id()      const noexcept;
    uint64_t slot_offset() const noexcept { return off_; }

    // Release all arena memory owned by this Doc (root block + all variable-length
    // payloads) using FreeMode::Delayed. Until ReclaimExpired() runs after
    // reclaim_delay_ns from the free timestamps, readers holding older snapshots may
    // still see valid data at those offsets.
    void Retire();

    int32_t  get_int32 (uint32_t fid) const;
    void     put_int32 (uint32_t fid, int32_t  v);
    int64_t  get_int64 (uint32_t fid) const;
    void     put_int64 (uint32_t fid, int64_t  v);
    float    get_float (uint32_t fid) const;
    void     put_float (uint32_t fid, float    v);
    double   get_double(uint32_t fid) const;
    void     put_double(uint32_t fid, double   v);

    std::string_view get_string(uint32_t fid) const;
    void             put_string(uint32_t fid, std::string_view v);

    uint32_t array_size(uint32_t fid) const;

    // Entire array as a contiguous view (pointer + length). Empty → {nullptr, 0}.
    std::pair<const int32_t*, uint32_t>  array_view_int32 (uint32_t fid) const;
    std::pair<const int64_t*, uint32_t>  array_view_int64 (uint32_t fid) const;
    std::pair<const float*,   uint32_t>  array_view_float(uint32_t fid) const;
    std::pair<const double*,  uint32_t>  array_view_double(uint32_t fid) const;

    int32_t  array_get_int32   (uint32_t fid, uint32_t i) const;
    void     array_put_int32   (uint32_t fid, const int32_t* data, uint32_t count);
    void     array_append_int32(uint32_t fid, int32_t val);

    int64_t  array_get_int64   (uint32_t fid, uint32_t i) const;
    void     array_put_int64   (uint32_t fid, const int64_t* data, uint32_t count);
    void     array_append_int64(uint32_t fid, int64_t val);

    float    array_get_float   (uint32_t fid, uint32_t i) const;
    void     array_put_float   (uint32_t fid, const float* data, uint32_t count);

    double   array_get_double  (uint32_t fid, uint32_t i) const;
    void     array_put_double  (uint32_t fid, const double* data, uint32_t count);

private:
    struct DocHeader {
        uint32_t doc_id;
        uint32_t n_slots;
    };

    struct Slot {
        uint64_t a;
        uint64_t b;
    };

    Slot* slots() const;
    Slot& slot (uint32_t fid) const;

    uint64_t array_capacity(uint32_t fid) const;

    template <typename T>
    T*   array_data       (uint32_t fid) const;
    template <typename T>
    void array_put_impl   (uint32_t fid, const T* data, uint32_t count);
    template <typename T>
    void array_append_impl(uint32_t fid, T val);

    alloc::Allocator* alloc_ = nullptr;
    uint64_t          off_   = 0;
};

}  // namespace index
}  // namespace yikv
