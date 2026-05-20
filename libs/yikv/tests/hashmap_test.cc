#include "container/hashmap.h"
#include "alloc/ft_allocator.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <map>
#include <string>
#include <unordered_map>

using yikv::alloc::AllocatorOptions;
using yikv::alloc::FtAllocator;
using yikv::container::HashMap;

// ─── helpers ──────────────────────────────────────────────────────────────────

static AllocatorOptions AnonArena(std::size_t size = 64 * 1024 * 1024) {
    AllocatorOptions opts;
    opts.arena_size = size;
    return opts;
}

static AllocatorOptions FileArena(const std::string& path,
                                   std::size_t size = 64 * 1024 * 1024) {
    AllocatorOptions opts;
    opts.path       = path;
    opts.arena_size = size;
    return opts;
}

using HMuu = HashMap<uint64_t, uint64_t>;
using HMss = HashMap<std::string, std::string>;
struct ConstHash {
    size_t operator()(uint64_t) const noexcept { return 0; }
};

// Verify snapshot matches a reference std::unordered_map.
template <class K, class V>
static void SnapshotMatchesRef(const typename HashMap<K, V>::Snapshot& snap,
                                const std::unordered_map<K, V>& ref) {
    EXPECT_EQ(snap.size(), ref.size());
    for (const auto& [k, v] : ref) {
        V out{};
        EXPECT_TRUE(snap.get(k, out)) << "key " << k << " missing";
        EXPECT_EQ(out, v);
    }
    // for_each covers no extra keys
    std::size_t visited = 0;
    snap.for_each([&](const K& k, const V& v2) {
        auto it = ref.find(k);
        ASSERT_NE(it, ref.end()) << "unexpected key";
        EXPECT_EQ(it->second, v2);
        ++visited;
    });
    EXPECT_EQ(visited, ref.size());
}

// ─── basic int→int ────────────────────────────────────────────────────────────

TEST(HashMapTest, PutAndGet) {
    FtAllocator alloc(AnonArena());
    HMuu hm(&alloc);

    hm.put(1, 100);
    hm.put(2, 200);
    hm.publish();

    auto snap = hm.acquire_snapshot();
    uint64_t out = 0;
    EXPECT_TRUE(snap.get(1, out)); EXPECT_EQ(out, 100u);
    EXPECT_TRUE(snap.get(2, out)); EXPECT_EQ(out, 200u);
    EXPECT_FALSE(snap.get(99, out));
}

TEST(HashMapTest, UpdateExistingKey) {
    FtAllocator alloc(AnonArena());
    HMuu hm(&alloc);

    hm.put(42, 1);
    hm.put(42, 2);
    hm.publish();

    auto snap = hm.acquire_snapshot();
    EXPECT_EQ(snap.size(), 1u);
    uint64_t out = 0;
    EXPECT_TRUE(snap.get(42, out));
    EXPECT_EQ(out, 2u);
}

TEST(HashMapTest, EraseKey) {
    FtAllocator alloc(AnonArena());
    HMuu hm(&alloc);

    hm.put(1, 10);
    hm.put(2, 20);
    hm.put(3, 30);
    hm.publish();

    EXPECT_TRUE(hm.erase(2));
    EXPECT_FALSE(hm.erase(99));
    hm.publish();

    auto snap = hm.acquire_snapshot();
    EXPECT_EQ(snap.size(), 2u);
    uint64_t out = 0;
    EXPECT_TRUE(snap.get(1, out));  EXPECT_EQ(out, 10u);
    EXPECT_FALSE(snap.get(2, out));
    EXPECT_TRUE(snap.get(3, out));  EXPECT_EQ(out, 30u);
}

TEST(HashMapTest, ContainsAfterErase) {
    FtAllocator alloc(AnonArena());
    HMuu hm(&alloc);

    hm.put(7, 77);
    hm.publish();

    EXPECT_TRUE(hm.acquire_snapshot().contains(7));
    hm.erase(7);
    hm.publish();
    EXPECT_FALSE(hm.acquire_snapshot().contains(7));
}

TEST(HashMapTest, EmptyMap) {
    FtAllocator alloc(AnonArena());
    HMuu hm(&alloc);

    auto snap = hm.acquire_snapshot();
    EXPECT_TRUE(snap.empty());
    EXPECT_EQ(snap.size(), 0u);
    uint64_t out = 0;
    EXPECT_FALSE(snap.get(1, out));
}

// ─── string → string ──────────────────────────────────────────────────────────

TEST(HashMapTest, StringKeys) {
    FtAllocator alloc(AnonArena());
    HMss hm(&alloc);

    hm.put("hello", "world");
    hm.put("foo",   "bar");
    hm.publish();

    auto snap = hm.acquire_snapshot();
    std::string out;
    EXPECT_TRUE(snap.get("hello", out)); EXPECT_EQ(out, "world");
    EXPECT_TRUE(snap.get("foo",   out)); EXPECT_EQ(out, "bar");
    EXPECT_FALSE(snap.get("missing", out));
}

TEST(HashMapTest, StringUpdate) {
    FtAllocator alloc(AnonArena());
    HMss hm(&alloc);

    hm.put("k", "v1");
    hm.put("k", "v2");
    hm.publish();

    std::string out;
    EXPECT_TRUE(hm.acquire_snapshot().get("k", out));
    EXPECT_EQ(out, "v2");
    EXPECT_EQ(hm.acquire_snapshot().size(), 1u);
}

// ─── bulk / rehash ────────────────────────────────────────────────────────────

TEST(HashMapTest, BulkInsertTriggeringRehash) {
    FtAllocator alloc(AnonArena());
    HMuu hm(&alloc, /*root_off=*/0, /*bucket_bits=*/4);  // start small: 16 buckets

    std::unordered_map<uint64_t, uint64_t> ref;
    for (uint64_t i = 0; i < 500; ++i) {
        hm.put(i, i * 10);
        ref[i] = i * 10;
    }
    hm.publish();

    SnapshotMatchesRef<uint64_t, uint64_t>(hm.acquire_snapshot(), ref);
}

TEST(HashMapTest, BulkEraseAfterBulkInsert) {
    FtAllocator alloc(AnonArena());
    HMuu hm(&alloc);

    for (uint64_t i = 0; i < 200; ++i) hm.put(i, i);
    hm.publish();

    for (uint64_t i = 0; i < 100; ++i) EXPECT_TRUE(hm.erase(i));
    hm.publish();

    auto snap = hm.acquire_snapshot();
    EXPECT_EQ(snap.size(), 100u);
    for (uint64_t i = 0; i < 100; ++i)   EXPECT_FALSE(snap.contains(i));
    for (uint64_t i = 100; i < 200; ++i) EXPECT_TRUE(snap.contains(i));
}

// ─── for_each ─────────────────────────────────────────────────────────────────

TEST(HashMapTest, ForEachVisitsAll) {
    FtAllocator alloc(AnonArena());
    HMuu hm(&alloc);

    std::unordered_map<uint64_t, uint64_t> ref;
    for (uint64_t i = 0; i < 50; ++i) { hm.put(i, i * 3); ref[i] = i * 3; }
    hm.publish();

    SnapshotMatchesRef<uint64_t, uint64_t>(hm.acquire_snapshot(), ref);
}

// ─── snapshot isolation ───────────────────────────────────────────────────────

TEST(HashMapTest, OldSnapshotNotAffectedByLaterWrite) {
    FtAllocator alloc(AnonArena());
    HMuu hm(&alloc);

    hm.put(1, 111);
    hm.publish();
    auto old_snap = hm.acquire_snapshot();

    hm.put(1, 999);
    hm.put(2, 222);
    hm.publish();

    // Old snapshot still sees stale values.
    uint64_t out = 0;
    EXPECT_TRUE(old_snap.get(1, out)); EXPECT_EQ(out, 111u);
    EXPECT_FALSE(old_snap.get(2, out));

    // New snapshot sees updated values.
    auto new_snap = hm.acquire_snapshot();
    EXPECT_TRUE(new_snap.get(1, out)); EXPECT_EQ(out, 999u);
    EXPECT_TRUE(new_snap.get(2, out)); EXPECT_EQ(out, 222u);
}

// ─── multi-publish batching ───────────────────────────────────────────────────

TEST(HashMapTest, MultiplePublishes) {
    FtAllocator alloc(AnonArena());
    HMuu hm(&alloc);

    for (int batch = 0; batch < 5; ++batch) {
        for (uint64_t i = 0; i < 20; ++i)
            hm.put(static_cast<uint64_t>(batch * 20 + i),
                   static_cast<uint64_t>(batch * 20 + i));
        hm.publish();
    }

    auto snap = hm.acquire_snapshot();
    EXPECT_EQ(snap.size(), 100u);
    for (uint64_t i = 0; i < 100; ++i) {
        uint64_t out = 0;
        EXPECT_TRUE(snap.get(i, out));
        EXPECT_EQ(out, i);
    }
}

TEST(HashMapTest, WritesInvisibleUntilPublish) {
    FtAllocator alloc(AnonArena());
    HMuu hm(&alloc);

    hm.put(10, 100);
    auto snap_before = hm.acquire_snapshot();
    uint64_t out = 0;
    EXPECT_FALSE(snap_before.get(10, out));

    hm.publish();
    auto snap_after = hm.acquire_snapshot();
    EXPECT_TRUE(snap_after.get(10, out));
    EXPECT_EQ(out, 100u);
}

// ─── persistence / mmap recovery ──────────────────────────────────────────────

TEST(HashMapTest, RecoveryFromMmapAfterReopen) {
    const std::string path = "/tmp/hashmap_recovery_test.dat";
    std::remove(path.c_str());

    uint64_t saved_root = 0;
    std::map<uint64_t, uint64_t> expected;

    // Phase 1: create, populate, close.
    {
        FtAllocator alloc(FileArena(path));
        HMuu hm(&alloc);
        saved_root = hm.root_offset();
        ASSERT_NE(saved_root, 0u);

        for (uint64_t i = 0; i < 300; ++i) {
            hm.put(i, i * 7);
            expected[i] = i * 7;
        }
        hm.publish();
        // allocator destructor flushes the mmap.
    }

    // Phase 2: reopen same file, recover.
    {
        FtAllocator alloc(FileArena(path));
        HMuu recovered(&alloc, saved_root);

        EXPECT_EQ(recovered.size(), expected.size());
        auto snap = recovered.acquire_snapshot();
        for (const auto& [k, v] : expected) {
            uint64_t out = 0;
            EXPECT_TRUE(snap.get(k, out)) << "missing key " << k;
            EXPECT_EQ(out, v);
        }
    }

    std::remove(path.c_str());
}

TEST(HashMapTest, RecoveryStringMapAfterReopen) {
    const std::string path = "/tmp/hashmap_str_recovery_test.dat";
    std::remove(path.c_str());

    uint64_t saved_root = 0;
    std::map<std::string, std::string> expected = {
        {"alpha", "1"}, {"beta", "2"}, {"gamma", "3"},
        {"delta", "4"}, {"epsilon", "5"},
    };

    // Phase 1
    {
        FtAllocator alloc(FileArena(path));
        HMss hm(&alloc);
        saved_root = hm.root_offset();
        for (const auto& [k, v] : expected) hm.put(k, v);
        hm.publish();
    }

    // Phase 2
    {
        FtAllocator alloc(FileArena(path));
        HMss recovered(&alloc, saved_root);

        EXPECT_EQ(recovered.size(), expected.size());
        auto snap = recovered.acquire_snapshot();
        for (const auto& [k, v] : expected) {
            std::string out;
            EXPECT_TRUE(snap.get(k, out)) << "missing key " << k;
            EXPECT_EQ(out, v);
        }
    }

    std::remove(path.c_str());
}

TEST(HashMapTest, RecoveryThenContinueWriting) {
    const std::string path = "/tmp/hashmap_recovery_continue_test.dat";
    std::remove(path.c_str());

    uint64_t saved_root = 0;

    {
        FtAllocator alloc(FileArena(path));
        HMuu hm(&alloc);
        hm.put(1, 10);
        hm.put(2, 20);
        hm.publish();
        saved_root = hm.root_offset();
    }

    {
        FtAllocator alloc(FileArena(path));
        HMuu recovered(&alloc, saved_root);
        recovered.put(3, 30);
        recovered.erase(1);
        recovered.publish();

        auto snap = recovered.acquire_snapshot();
        uint64_t out = 0;
        EXPECT_FALSE(snap.get(1, out));
        EXPECT_TRUE(snap.get(2, out));
        EXPECT_EQ(out, 20u);
        EXPECT_TRUE(snap.get(3, out));
        EXPECT_EQ(out, 30u);
        EXPECT_EQ(snap.size(), 2u);
    }

    std::remove(path.c_str());
}

// ─── edge cases ───────────────────────────────────────────────────────────────

TEST(HashMapTest, EraseNonexistentKey) {
    FtAllocator alloc(AnonArena());
    HMuu hm(&alloc);
    EXPECT_FALSE(hm.erase(42));
    hm.put(1, 1);
    hm.publish();
    EXPECT_FALSE(hm.erase(42));
}

TEST(HashMapTest, PutAfterErase) {
    FtAllocator alloc(AnonArena());
    HMuu hm(&alloc);

    hm.put(5, 50);
    hm.publish();
    hm.erase(5);
    hm.put(5, 55);
    hm.publish();

    uint64_t out = 0;
    EXPECT_TRUE(hm.acquire_snapshot().get(5, out));
    EXPECT_EQ(out, 55u);
    EXPECT_EQ(hm.size(), 1u);
}

TEST(HashMapTest, HashCollisionChaining) {
    // Force many keys into the same bucket by using the same hash value.
    // We do this indirectly by using a tiny map (2 buckets) and inserting
    // many keys, guaranteeing overflow chains.
    FtAllocator alloc(AnonArena());
    HashMap<uint64_t, uint64_t> hm(&alloc, 0, /*bucket_bits=*/1);

    std::unordered_map<uint64_t, uint64_t> ref;
    for (uint64_t i = 0; i < 40; ++i) { hm.put(i, i + 1); ref[i] = i + 1; }
    hm.publish();
    SnapshotMatchesRef<uint64_t, uint64_t>(hm.acquire_snapshot(), ref);
}

TEST(HashMapTest, HashCollisionWithConstantHasher) {
    FtAllocator alloc(AnonArena());
    HashMap<uint64_t, uint64_t, ConstHash> hm(&alloc, 0, /*bucket_bits=*/4);

    for (uint64_t i = 0; i < 120; ++i) hm.put(i, i * 11);
    hm.publish();

    auto snap = hm.acquire_snapshot();
    EXPECT_EQ(snap.size(), 120u);
    for (uint64_t i = 0; i < 120; ++i) {
        uint64_t out = 0;
        EXPECT_TRUE(snap.get(i, out));
        EXPECT_EQ(out, i * 11);
    }

    for (uint64_t i = 0; i < 40; ++i) EXPECT_TRUE(hm.erase(i));
    hm.publish();

    auto snap2 = hm.acquire_snapshot();
    EXPECT_EQ(snap2.size(), 80u);
    for (uint64_t i = 0; i < 40; ++i) EXPECT_FALSE(snap2.contains(i));
    for (uint64_t i = 40; i < 120; ++i) {
        uint64_t out = 0;
        EXPECT_TRUE(snap2.get(i, out));
        EXPECT_EQ(out, i * 11);
    }
}
