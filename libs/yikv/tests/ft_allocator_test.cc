#include "alloc/ft_allocator.h"
#include "alloc/system_allocator.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <thread>
#include <vector>
#include <unistd.h>

#include "gtest/gtest.h"

namespace yikv::alloc {
namespace {

AllocatorOptions AnonymousArena(std::uint64_t arena_bytes = 4ull * 1024 * 1024) {
    AllocatorOptions o;
    o.path.clear();
    o.arena_size = arena_bytes;
    o.page_size  = 4096;
    o.mode       = AllocatorMode::Concurrent;
    return o;
}

TEST(FtAllocatorTest, OpenCloseAnonymous) {
    FtAllocator a;
    EXPECT_FALSE(a.IsOpen());
    a.Open(AnonymousArena());
    EXPECT_TRUE(a.IsOpen());
    EXPECT_NE(a.BaseAddress(), nullptr);
    a.Close();
    EXPECT_FALSE(a.IsOpen());
}

TEST(FtAllocatorTest, MallocFreeSmallCheckConsistency) {
    FtAllocator a(AnonymousArena());
    void* p = a.Malloc(64);
    ASSERT_NE(p, nullptr);
    std::memset(p, 0xAB, 64);
    a.Free(p);
    a.CheckConsistency();
}

TEST(FtAllocatorTest, MallocLargeImmediateFree) {
    FtAllocator a(AnonymousArena());
    // Larger than max size-class block (page-sized class is 4096 B total).
    void* p = a.Malloc(5000);
    ASSERT_NE(p, nullptr);
    std::memset(p, 0xCD, 5000);
    a.Free(p);
    a.CheckConsistency();
}

TEST(FtAllocatorTest, PtrOffsetRoundTrip) {
    FtAllocator a(AnonymousArena());
    void* p = a.Malloc(32);
    ASSERT_NE(p, nullptr);
    std::uint64_t off = a.PtrToOffset(p);
    EXPECT_NE(off, 0u);
    EXPECT_EQ(a.OffsetToPtr(off), p);

    MmapPtr<int> mp = a.ToMmapPtr(static_cast<int*>(p));
    EXPECT_EQ(mp.offset, off);
    *a.FromMmapPtr(mp) = 42;
    EXPECT_EQ(*static_cast<int*>(p), 42);

    a.Free(p);
}

TEST(FtAllocatorTest, NewDelete) {
    FtAllocator a(AnonymousArena());
    struct Pod {
        int x;
        double y;
        Pod(int a, double b) : x(a), y(b) {}
    };
    Pod* p = a.New<Pod>(7, 3.25);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(p->x, 7);
    EXPECT_DOUBLE_EQ(p->y, 3.25);
    a.Delete(p);
    a.CheckConsistency();
}

TEST(FtAllocatorTest, DelayedFreeReclaim) {
    // Use reclaim_delay_ns=0 so blocks are eligible for reclamation immediately.
    AllocatorOptions opts = AnonymousArena();
    opts.reclaim_delay_ns = 0;
    FtAllocator a(opts);
    void* p = a.Malloc(128);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(a.CurrentEpoch(), 1u);
    a.Free(p, FreeMode::Delayed);
    auto s = a.GetStats();
    EXPECT_GT(s.delayed_count, 0u);

    std::size_t n = a.ReclaimExpired();
    EXPECT_GE(n, 1u);
    s = a.GetStats();
    EXPECT_EQ(s.delayed_count, 0u);

    void* q = a.Malloc(128);
    ASSERT_NE(q, nullptr);
    a.Free(q);
    a.CheckConsistency();
}

TEST(FtAllocatorTest, DoubleFreeThrows) {
    FtAllocator a(AnonymousArena());
    void* p = a.Malloc(32);
    ASSERT_NE(p, nullptr);
    a.Free(p);
    EXPECT_THROW(a.Free(p), std::runtime_error);
}

TEST(FtAllocatorTest, PtrToOffsetOutsideArenaThrows) {
    FtAllocator a(AnonymousArena());
    int stack_x = 0;
    EXPECT_THROW(a.PtrToOffset(&stack_x), std::invalid_argument);
}

TEST(FtAllocatorTest, ArenaTooSmallThrows) {
    FtAllocator a;
    AllocatorOptions o = AnonymousArena();
    o.arena_size = 512 * 1024;
    EXPECT_THROW(a.Open(o), std::invalid_argument);
}

TEST(FtAllocatorTest, MmapStdAllocatorVector) {
    FtAllocator a(AnonymousArena());
    MmapStdAllocator<int> alloc(&a);
    std::vector<int, MmapStdAllocator<int>> v(alloc);
    for (int i = 0; i < 100; ++i) v.push_back(i);
    for (int i = 0; i < 100; ++i) EXPECT_EQ(v[i], i);
    v.clear();
    v.shrink_to_fit();
    a.FlushTlc();
    a.CheckConsistency();
}

TEST(FtAllocatorTest, SingleWriterModeSmoke) {
    AllocatorOptions o = AnonymousArena();
    o.mode = AllocatorMode::SingleWriter;
    FtAllocator a(o);
    void* p = a.Malloc(48);
    ASSERT_NE(p, nullptr);
    a.PublishFence();
    a.Free(p);
    a.CheckConsistency();
}

TEST(FtAllocatorTest, ConcurrentMallocFreeStress) {
    FtAllocator a(AnonymousArena(8ull * 1024 * 1024));
    constexpr int kThreads = 4;
    constexpr int kIters   = 200;
    std::atomic<int> failures{0};

    auto worker = [&] {
        try {
            for (int i = 0; i < kIters; ++i) {
                void* p = a.Malloc(32 + (i % 8) * 16);
                if (!p) {
                    failures.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
                *static_cast<std::uint32_t*>(p) = 0xDEADBEEFu;
                a.Free(p);
            }
        } catch (...) {
            failures.fetch_add(1000, std::memory_order_relaxed);
        }
    };

    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) threads.emplace_back(worker);
    for (auto& th : threads) th.join();

    EXPECT_EQ(failures.load(), 0);
    a.CheckConsistency();
}

TEST(SystemAllocatorTest, BasicMallocFreeAndEpoch) {
    AllocatorOptions o;
    SystemAllocator a(o);
    ASSERT_TRUE(a.IsOpen());

    void* p = a.Malloc(64);
    ASSERT_NE(p, nullptr);
    std::memset(p, 0xAB, 64);

    auto before = a.CurrentEpoch();
    auto after = a.AdvanceEpoch();
    EXPECT_EQ(after, before + 1);

    a.PublishFence();
    a.Free(p);
    a.CheckConsistency();
}

TEST(FtAllocatorTest, FileBackedSegmentGrowth) {
    const std::string base = "/tmp/ft_allocator_growth_test.dat";
    std::remove(base.c_str());
    std::remove((base + ".seg1").c_str());
    std::remove((base + ".seg2").c_str());

    AllocatorOptions o;
    o.path = base;
    o.page_size = 4096;
    o.arena_size = 32ull * 1024 * 1024;
    o.segment_size = 32ull * 1024 * 1024;
    o.max_arena_size = 96ull * 1024 * 1024;
    o.mode = AllocatorMode::SingleWriter;

    FtAllocator a(o);
    std::vector<void*> ptrs;
    for (int i = 0; i < 40; ++i) {
        // Each allocation rounds to page-sized/large buckets; total drives growth.
        ptrs.push_back(a.Malloc(2ull * 1024 * 1024));
    }
    for (void* p : ptrs) a.Free(p);
    a.CheckConsistency();

    EXPECT_EQ(access((base + ".seg1").c_str(), F_OK), 0);

    std::remove(base.c_str());
    std::remove((base + ".seg1").c_str());
    std::remove((base + ".seg2").c_str());
}

}  // namespace
}  // namespace yikv::alloc
