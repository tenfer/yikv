#pragma once

#include "src/alloc/allocator.h"

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace yikv {
namespace alloc {

class SystemAllocator : public Allocator {
public:
    SystemAllocator() = default;
    explicit SystemAllocator(const AllocatorOptions& options) { Open(options); }
    ~SystemAllocator() override = default;

    void Open(const AllocatorOptions& options) override;
    void Close() noexcept override;
    bool IsOpen() const noexcept override;
    void* BaseAddress() const noexcept override;

    void* Malloc(std::size_t size) override;
    void  Free(void* ptr, FreeMode mode = FreeMode::Immediate) override;

    std::uint64_t PtrToOffset(const void* ptr) const override;
    void*         OffsetToPtr(std::uint64_t offset) const override;

    std::uint64_t AdvanceEpoch() override;
    std::uint64_t CurrentEpoch() const override;
    std::size_t   ReclaimExpired() override;
    std::uint64_t ReclaimDelayNs() const noexcept override { return 0; }
    void PublishFence() noexcept override;
    void FlushTlc() override;
    AllocatorStats GetStats() const override;
    void CheckConsistency() const override;

private:
    bool open_ = false;
    std::atomic<std::uint64_t> current_epoch_{1};
    std::atomic<std::uint64_t> used_bytes_{0};
    std::atomic<std::uint64_t> alloc_count_{0};
    std::atomic<std::uint64_t> free_count_{0};
    AllocatorOptions options_{};
};

}  // namespace alloc
}  // namespace yikv
