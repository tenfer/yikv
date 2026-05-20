#include "container/concurrent_hashmap.h"
#include "alloc/allocator.h"
#include "alloc/ft_allocator.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdio>
#include <map>
#include <string>
#include <thread>
#include <vector>

using yikv::alloc::AllocatorMode;
using yikv::alloc::AllocatorOptions;
using yikv::alloc::FtAllocator;
using yikv::container::ConcurrentHashMap;

namespace {

AllocatorOptions ConcurrentArena(std::size_t size = 64 * 1024 * 1024) {
    AllocatorOptions opts;
    opts.arena_size = size;
    opts.mode       = AllocatorMode::Concurrent;
    return opts;
}

AllocatorOptions ConcurrentFileArena(const std::string& path,
                                     std::size_t       size = 64 * 1024 * 1024) {
    AllocatorOptions opts;
    opts.path       = path;
    opts.arena_size = size;
    opts.mode       = AllocatorMode::Concurrent;
    return opts;
}

using CMuu = ConcurrentHashMap<uint64_t, uint64_t>;

}  // namespace

TEST(ConcurrentHashMapTest, PutGetEraseSingleThread) {
    FtAllocator alloc(ConcurrentArena());
    CMuu        hm(&alloc, 0, 6, 4);  // 64 buckets, 16 stripes

    uint64_t v = 0;
    EXPECT_FALSE(hm.get(1u, v));

    hm.put(1u, 100u);
    EXPECT_TRUE(hm.get(1u, v));
    EXPECT_EQ(v, 100u);
    EXPECT_EQ(hm.size(), 1u);

    hm.put(1u, 200u);
    EXPECT_TRUE(hm.get(1u, v));
    EXPECT_EQ(v, 200u);
    EXPECT_EQ(hm.size(), 1u);

    hm.put(2u, 20u);
    EXPECT_EQ(hm.size(), 2u);

    EXPECT_TRUE(hm.erase(1u));
    EXPECT_FALSE(hm.get(1u, v));
    EXPECT_EQ(hm.size(), 1u);
    EXPECT_TRUE(hm.get(2u, v));
    EXPECT_EQ(v, 20u);
}

TEST(ConcurrentHashMapTest, RehashExpandsTable) {
    FtAllocator alloc(ConcurrentArena());
    // bucket_bits=2 => 4 buckets; kLoadFactor=2 => rehash at >= 8 entries
    CMuu hm(&alloc, 0, 2, 2);

    for (uint64_t i = 0; i < 32; ++i) {
        hm.put(i, i * 10);
    }
    EXPECT_EQ(hm.size(), 32u);
    for (uint64_t i = 0; i < 32; ++i) {
        uint64_t out = 0;
        ASSERT_TRUE(hm.get(i, out)) << "key " << i;
        EXPECT_EQ(out, i * 10);
    }
}

TEST(ConcurrentHashMapTest, ConcurrentWritersThenReadAll) {
    FtAllocator alloc(ConcurrentArena());
    CMuu        hm(&alloc, 0, 8, 5);

    constexpr int           kThreads = 8;
    constexpr int             kPer     = 500;
    std::vector<std::thread> th;
    th.reserve(static_cast<std::size_t>(kThreads));
    for (int t = 0; t < kThreads; ++t) {
        th.emplace_back([&, t]() {
            for (int i = 0; i < kPer; ++i) {
                const uint64_t k = static_cast<uint64_t>(t * kPer + i);
                hm.put(k, k * 7 + 3);
            }
        });
    }
    for (auto& x : th)
        x.join();

    const auto expect_count = static_cast<std::size_t>(kThreads * kPer);
    EXPECT_EQ(hm.size(), expect_count);
    for (uint64_t k = 0; k < kThreads * kPer; ++k) {
        uint64_t out = 0;
        ASSERT_TRUE(hm.get(k, out));
        EXPECT_EQ(out, k * 7 + 3);
    }
}

TEST(ConcurrentHashMapTest, ConcurrentReadersAndWriters) {
    FtAllocator alloc(ConcurrentArena());
    CMuu        hm(&alloc, 0, 10, 6);

    std::atomic<bool> stop{false};
    constexpr int      kWriters = 4;
    constexpr int      kReaders = 4;
    constexpr int      kBatch   = 2000;

    std::vector<std::thread> th;

    for (int w = 0; w < kWriters; ++w) {
        th.emplace_back([&, w]() {
            const uint64_t base = static_cast<uint64_t>(w) * 100000 + 1;
            for (int i = 0; i < kBatch; ++i) {
                const uint64_t k = base + static_cast<uint64_t>(i);
                hm.put(k, k ^ 0x9e3779b97f4a7c15ULL);
            }
        });
    }

    for (int r = 0; r < kReaders; ++r) {
        th.emplace_back([&]() {
            while (!stop.load(std::memory_order_acquire)) {
                uint64_t out = 0;
                hm.get(static_cast<uint64_t>(r * 17 + 1), out);
            }
        });
    }

    for (int w = 0; w < kWriters; ++w)
        th[static_cast<std::size_t>(w)].join();

    stop.store(true, std::memory_order_release);
    for (std::size_t i = static_cast<std::size_t>(kWriters);
         i < th.size(); ++i)
        th[i].join();

    EXPECT_EQ(hm.size(),
              static_cast<std::size_t>(kWriters * kBatch));
    for (int w = 0; w < kWriters; ++w) {
        const uint64_t base = static_cast<uint64_t>(w) * 100000 + 1;
        for (int i = 0; i < kBatch; ++i) {
            const uint64_t k   = base + static_cast<uint64_t>(i);
            uint64_t       out = 0;
            ASSERT_TRUE(hm.get(k, out));
            EXPECT_EQ(out, k ^ 0x9e3779b97f4a7c15ULL);
        }
    }
}

TEST(ConcurrentHashMapTest, RecoveryFromMmapAfterReopen) {
    const std::string path = "/tmp/concurrent_hashmap_recovery_test.dat";
    std::remove(path.c_str());

    uint64_t         saved_hdr = 0;
    constexpr uint8_t kStripe  = 5;
    std::map<uint64_t, uint64_t> expected;

    {
        FtAllocator alloc(ConcurrentFileArena(path));
        CMuu        hm(&alloc, 0, 8, kStripe);
        saved_hdr = hm.head_region_offset();
        ASSERT_NE(saved_hdr, 0u);

        for (uint64_t i = 0; i < 400; ++i) {
            hm.put(i, i * 11 + 1);
            expected[i] = i * 11 + 1;
        }
    }

    {
        FtAllocator alloc(ConcurrentFileArena(path));
        CMuu        recovered(&alloc, saved_hdr, 8, kStripe);

        EXPECT_EQ(recovered.size(), expected.size());
        for (const auto& [k, v] : expected) {
            uint64_t out = 0;
            EXPECT_TRUE(recovered.get(k, out)) << "missing " << k;
            EXPECT_EQ(out, v);
        }
    }

    std::remove(path.c_str());
}

TEST(ConcurrentHashMapTest, RecoveryRejectsWrongStripeShift) {
    const std::string path = "/tmp/concurrent_hashmap_stripe_mismatch.dat";
    std::remove(path.c_str());

    uint64_t saved_hdr = 0;
    {
        FtAllocator alloc(ConcurrentFileArena(path));
        CMuu        hm(&alloc, 0, 4, 3);
        saved_hdr = hm.head_region_offset();
        hm.put(1u, 1u);
    }

    {
        FtAllocator alloc(ConcurrentFileArena(path));
        EXPECT_THROW((CMuu(&alloc, saved_hdr, 4, 4)),
                     std::runtime_error);
    }

    std::remove(path.c_str());
}

TEST(ConcurrentHashMapTest, LockFreeReadsPutAndEraseThrow) {
    FtAllocator alloc(ConcurrentArena());
    CMuu        hm(&alloc, 0, 8, 4);
    hm.put(42u, 1u);
    hm.enable_lock_free_reads();
    EXPECT_TRUE(hm.lock_free_reads_enabled());
    uint64_t v = 0;
    EXPECT_TRUE(hm.get(42u, v));
    EXPECT_EQ(v, 1u);
    EXPECT_THROW(hm.put(43u, 2u), std::logic_error);
    EXPECT_THROW(hm.erase(42u), std::logic_error);
    hm.disable_lock_free_reads();
    EXPECT_FALSE(hm.lock_free_reads_enabled());
    hm.put(43u, 3u);
    EXPECT_TRUE(hm.erase(42u));
}

TEST(ConcurrentHashMapTest, LockFreeReadsConcurrentGets) {
    FtAllocator alloc(ConcurrentArena());
    CMuu        hm(&alloc, 0, 10, 5);
    constexpr uint64_t kN = 2000;
    for (uint64_t i = 0; i < kN; ++i) {
        hm.put(i, i * 13 + 7);
    }
    hm.enable_lock_free_reads();

    constexpr int           kReaders = 8;
    std::vector<std::thread> th;
    for (int r = 0; r < kReaders; ++r) {
        th.emplace_back([&, r]() {
            for (uint64_t j = 0; j < kN; ++j) {
                const uint64_t k = (j + static_cast<uint64_t>(r) * 997u) % kN;
                uint64_t       out = 0;
                EXPECT_TRUE(hm.get(k, out));
                EXPECT_EQ(out, k * 13 + 7);
            }
        });
    }
    for (auto& t : th) {
        t.join();
    }
}
