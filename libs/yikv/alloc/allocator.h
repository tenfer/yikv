#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>
#include <string>
#include <type_traits>
#include <utility>

namespace yikv {
namespace alloc {

enum class AllocatorMode {
    Concurrent,
    SingleWriter,
};

struct AllocatorOptions {
    std::string   path;
    std::uint64_t arena_size        = 256ull * 1024 * 1024;
    // Optional growth settings (file-backed mode):
    // - segment_size: size per data file segment (default 1 GiB)
    // - max_arena_size: virtual max capacity across all segments (default = arena_size)
    std::uint64_t segment_size      = 1ull * 1024 * 1024 * 1024;
    std::uint64_t max_arena_size    = 0;
    std::uint32_t page_size         = 4096;
    AllocatorMode mode              = AllocatorMode::Concurrent;
    bool          create_if_missing = true;
    // How long (nanoseconds) a Delayed-freed block must age before it is
    // eligible for reclamation.  Default: 5 seconds.  Callers with known-short
    // reader lifetimes may reduce this; set to 0 to reclaim immediately.
    std::uint64_t reclaim_delay_ns  = 5'000'000'000ULL;
};

struct AllocatorStats {
    std::uint64_t arena_size       = 0;
    std::uint64_t data_offset      = 0;
    std::uint64_t page_size        = 0;
    std::uint64_t page_count       = 0;
    std::uint64_t free_page_count  = 0;
    std::uint64_t used_bytes       = 0;
    std::uint64_t allocation_count = 0;
    std::uint64_t free_count       = 0;
    std::uint64_t delayed_count    = 0;
    std::uint64_t current_epoch    = 0;
};

template <class T>
struct MmapPtr {
    std::uint64_t offset = 0;

    bool empty() const noexcept { return offset == 0; }

    T* get(void* base) const noexcept {
        if (offset == 0) return nullptr;
        return reinterpret_cast<T*>(static_cast<char*>(base) + offset);
    }
    const T* get(const void* base) const noexcept {
        if (offset == 0) return nullptr;
        return reinterpret_cast<const T*>(static_cast<const char*>(base) + offset);
    }
};

enum class FreeMode {
    Immediate,
    Delayed,
};

class Allocator {
public:
    virtual ~Allocator() = default;

    virtual void Open(const AllocatorOptions& options) = 0;
    virtual void Close() noexcept = 0;
    virtual bool IsOpen() const noexcept = 0;
    virtual void* BaseAddress() const noexcept = 0;

    virtual void* Malloc(std::size_t size) = 0;
    virtual void  Free(void* ptr, FreeMode mode = FreeMode::Immediate) = 0;

    template <class T, class... Args>
    T* New(Args&&... args) {
        void* mem = Malloc(sizeof(T));
        try {
            return ::new (mem) T(std::forward<Args>(args)...);
        } catch (...) {
            Free(mem);
            throw;
        }
    }

    template <class T>
    void Delete(T* ptr, FreeMode mode = FreeMode::Immediate) {
        if (ptr == nullptr) return;
        ptr->~T();
        Free(ptr, mode);
    }

    virtual std::uint64_t PtrToOffset(const void* ptr) const = 0;
    virtual void*         OffsetToPtr(std::uint64_t offset) const = 0;

    template <class T>
    MmapPtr<T> ToMmapPtr(T* ptr) const {
        return MmapPtr<T>{PtrToOffset(ptr)};
    }
    template <class T>
    T* FromMmapPtr(MmapPtr<T> mp) const {
        return static_cast<T*>(OffsetToPtr(mp.offset));
    }

    virtual std::uint64_t AdvanceEpoch() = 0;
    virtual std::uint64_t CurrentEpoch() const = 0;

    // Reclaim all Delayed-freed blocks whose age exceeds the configured
    // reclaim_delay_ns.  Called automatically inside Malloc(); callers may
    // invoke it explicitly to force an early sweep.
    virtual std::size_t   ReclaimExpired() = 0;
    virtual std::uint64_t ReclaimDelayNs() const noexcept = 0;

    virtual void PublishFence() noexcept = 0;
    virtual void FlushTlc() = 0;
    virtual AllocatorStats GetStats() const = 0;
    virtual void CheckConsistency() const = 0;
};

}  // namespace alloc
}  // namespace yikv
