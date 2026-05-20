#pragma once

#include "alloc/allocator.h"

#include <memory>

namespace yikv {
namespace alloc {

// Read reserved virtual size from an existing arena file (offset 0 header).
// Returns false if the file is missing or not a v3 FtAllocator arena.
bool PeekFtArenaReservedBytes(const std::string& arena_path, std::uint64_t* out_reserved);

class FtAllocator : public Allocator {
public:
    FtAllocator();
    explicit FtAllocator(const AllocatorOptions& options);
    ~FtAllocator();

    FtAllocator(const FtAllocator&)            = delete;
    FtAllocator& operator=(const FtAllocator&) = delete;
    FtAllocator(FtAllocator&&) noexcept;
    FtAllocator& operator=(FtAllocator&&) noexcept;

    void Open(const AllocatorOptions& options) override;
    void Close() noexcept override;
    bool IsOpen() const noexcept override;

    // Base address of the mapped region – use with MmapPtr::get() to avoid
    // going through the allocator on every dereference.
    void* BaseAddress() const noexcept override;

    void* Malloc(std::size_t size) override;
    void  Free(void* ptr, FreeMode mode = FreeMode::Immediate) override;

    std::uint64_t PtrToOffset(const void* ptr) const override;
    void*         OffsetToPtr(std::uint64_t offset) const override;

    // Epoch management (still used internally for publisher sequencing).
    std::uint64_t AdvanceEpoch() override;
    std::uint64_t CurrentEpoch() const override;

    // Reclaim all Delayed-freed blocks older than reclaim_delay_ns.
    std::size_t   ReclaimExpired() override;
    std::uint64_t ReclaimDelayNs() const noexcept override;

    // SingleWriter mode: emit a release memory fence so readers see all
    // writes done by the writer before this call.
    void PublishFence() noexcept override;

    // Flush the calling thread's thread-local cache back to the shared
    // free lists.
    void FlushTlc() override;

    AllocatorStats GetStats() const override;
    void           CheckConsistency() const override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ---------------------------------------------------------------------------
// MmapStdAllocator<T>
//
// C++ allocator adaptor so STL containers can place their internal storage
// inside the mmap arena.
// ---------------------------------------------------------------------------
template <class T>
class MmapStdAllocator {
public:
    using value_type      = T;
    using pointer         = T*;
    using const_pointer   = const T*;
    using size_type       = std::size_t;
    using difference_type = std::ptrdiff_t;

    template <class U>
    struct rebind { using other = MmapStdAllocator<U>; };

    MmapStdAllocator() noexcept = default;
    explicit MmapStdAllocator(Allocator* a) noexcept : allocator_(a) {}

    template <class U>
    MmapStdAllocator(const MmapStdAllocator<U>& o) noexcept
        : allocator_(o.allocator()) {}

    T* allocate(std::size_t n) {
        if (!allocator_) throw std::bad_alloc();
        if (n > static_cast<std::size_t>(-1) / sizeof(T))
            throw std::bad_array_new_length();
        return static_cast<T*>(allocator_->Malloc(n * sizeof(T)));
    }

    void deallocate(T* ptr, std::size_t) noexcept {
        if (allocator_ && ptr) {
            try { allocator_->Free(ptr); } catch (...) {}
        }
    }

    Allocator* allocator() const noexcept { return allocator_; }

    template <class U>
    bool operator==(const MmapStdAllocator<U>& o) const noexcept {
        return allocator_ == o.allocator();
    }
    template <class U>
    bool operator!=(const MmapStdAllocator<U>& o) const noexcept {
        return !(*this == o);
    }

private:
    template <class U> friend class MmapStdAllocator;
    Allocator* allocator_ = nullptr;
};

}  // namespace alloc
}  // namespace yikv
