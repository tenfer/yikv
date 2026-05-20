#include "alloc/system_allocator.h"

#include <atomic>
#include <cstdlib>
#include <new>
#include <stdexcept>

namespace yikv {
namespace alloc {

void SystemAllocator::Open(const AllocatorOptions& options) {
    options_ = options;
    current_epoch_.store(1, std::memory_order_relaxed);
    used_bytes_.store(0, std::memory_order_relaxed);
    alloc_count_.store(0, std::memory_order_relaxed);
    free_count_.store(0, std::memory_order_relaxed);
    open_ = true;
}

void SystemAllocator::Close() noexcept {
    open_ = false;
}

bool SystemAllocator::IsOpen() const noexcept { return open_; }

void* SystemAllocator::BaseAddress() const noexcept { return nullptr; }

void* SystemAllocator::Malloc(std::size_t size) {
    if (!open_) throw std::runtime_error("allocator not open");
    void* p = std::malloc(size > 0 ? size : 1);
    if (!p) throw std::bad_alloc();
    alloc_count_.fetch_add(1, std::memory_order_relaxed);
    used_bytes_.fetch_add(size, std::memory_order_relaxed);
    return p;
}

void SystemAllocator::Free(void* ptr, FreeMode) {
    if (!ptr) return;
    if (!open_) throw std::runtime_error("allocator not open");
    free_count_.fetch_add(1, std::memory_order_relaxed);
    std::free(ptr);
}

std::uint64_t SystemAllocator::PtrToOffset(const void* ptr) const {
    return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(ptr));
}

void* SystemAllocator::OffsetToPtr(std::uint64_t offset) const {
    return reinterpret_cast<void*>(static_cast<std::uintptr_t>(offset));
}

std::uint64_t SystemAllocator::AdvanceEpoch() {
    return current_epoch_.fetch_add(1, std::memory_order_acq_rel) + 1;
}

std::uint64_t SystemAllocator::CurrentEpoch() const {
    return current_epoch_.load(std::memory_order_acquire);
}

std::size_t SystemAllocator::ReclaimExpired() { return 0; }

void SystemAllocator::PublishFence() noexcept {
    std::atomic_thread_fence(std::memory_order_release);
}

void SystemAllocator::FlushTlc() {}

AllocatorStats SystemAllocator::GetStats() const {
    AllocatorStats s;
    s.arena_size = options_.arena_size;
    s.page_size = options_.page_size;
    s.used_bytes = used_bytes_.load(std::memory_order_relaxed);
    s.allocation_count = alloc_count_.load(std::memory_order_relaxed);
    s.free_count = free_count_.load(std::memory_order_relaxed);
    s.current_epoch = CurrentEpoch();
    return s;
}

void SystemAllocator::CheckConsistency() const {
    if (!open_) throw std::runtime_error("allocator not open");
}

}  // namespace alloc
}  // namespace yikv
