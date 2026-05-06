#include "src/container/bitmap.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

using yikv::alloc::AllocatorOptions;
using yikv::alloc::FtAllocator;
using yikv::container::Bitmap;

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

TEST(BitmapTest, AddContainsRemove) {
    FtAllocator alloc(AnonArena());
    Bitmap bm(&alloc);

    EXPECT_TRUE(bm.IsEmpty());
    bm.Add(1);
    bm.Add(2);
    bm.Add(2);
    EXPECT_EQ(bm.Cardinality(), 2u);
    EXPECT_TRUE(bm.Contains(1));
    EXPECT_TRUE(bm.Contains(2));
    EXPECT_FALSE(bm.Contains(3));

    bm.Remove(1);
    EXPECT_FALSE(bm.Contains(1));
    EXPECT_EQ(bm.Cardinality(), 1u);

    bm.Remove(42);
    EXPECT_EQ(bm.Cardinality(), 1u);
}

TEST(BitmapTest, BulkAddSortedWithDuplicates) {
    FtAllocator alloc(AnonArena());
    Bitmap bm(&alloc);

    std::vector<uint32_t> vals = {
        1, 2, 2, 3, 65535, 65536, 65536, 70000, 999999
    };
    std::sort(vals.begin(), vals.end());
    bm.BulkAdd(vals.data(), vals.size());

    std::vector<uint32_t> expect = {1, 2, 3, 65535, 65536, 70000, 999999};
    EXPECT_EQ(bm.Cardinality(), expect.size());
    for (uint32_t v : expect) EXPECT_TRUE(bm.Contains(v));
}

TEST(BitmapTest, ForEachYieldsSortedValues) {
    FtAllocator alloc(AnonArena());
    Bitmap bm(&alloc);

    std::vector<uint32_t> vals = {10, 1, 70000, 65536, 2, 3};
    std::sort(vals.begin(), vals.end());
    bm.BulkAdd(vals.data(), vals.size());

    std::vector<uint32_t> got;
    bm.ForEach([&](uint32_t v) { got.push_back(v); });

    EXPECT_EQ(got, vals);
}

TEST(BitmapTest, SetOperationsValueReturning) {
    FtAllocator alloc(AnonArena());
    Bitmap a(&alloc);
    Bitmap b(&alloc);

    for (uint32_t v : std::vector<uint32_t>{1, 2, 3, 100000}) a.Add(v);
    for (uint32_t v : std::vector<uint32_t>{3, 4, 5, 100000}) b.Add(v);

    Bitmap u = a.Or(b);
    Bitmap i = a.And(b);
    Bitmap x = a.Xor(b);
    Bitmap d = a.AndNot(b);

    for (uint32_t v : std::vector<uint32_t>{1, 2, 3, 4, 5, 100000})
        EXPECT_TRUE(u.Contains(v));
    EXPECT_EQ(u.Cardinality(), 6u);

    for (uint32_t v : std::vector<uint32_t>{3, 100000}) EXPECT_TRUE(i.Contains(v));
    EXPECT_EQ(i.Cardinality(), 2u);

    for (uint32_t v : std::vector<uint32_t>{1, 2, 4, 5}) EXPECT_TRUE(x.Contains(v));
    EXPECT_EQ(x.Cardinality(), 4u);

    for (uint32_t v : std::vector<uint32_t>{1, 2}) EXPECT_TRUE(d.Contains(v));
    EXPECT_EQ(d.Cardinality(), 2u);
}

TEST(BitmapTest, InPlaceSetOperations) {
    FtAllocator alloc(AnonArena());
    Bitmap a(&alloc);
    Bitmap b(&alloc);

    for (uint32_t v : std::vector<uint32_t>{1, 2, 3}) a.Add(v);
    for (uint32_t v : std::vector<uint32_t>{3, 4}) b.Add(v);

    a.OrWith(b);
    EXPECT_EQ(a.Cardinality(), 4u);
    for (uint32_t v : std::vector<uint32_t>{1, 2, 3, 4}) EXPECT_TRUE(a.Contains(v));

    a.AndWith(b);
    EXPECT_EQ(a.Cardinality(), 2u);
    EXPECT_TRUE(a.Contains(3));
    EXPECT_TRUE(a.Contains(4));
    EXPECT_FALSE(a.Contains(1));
}

TEST(BitmapTest, SerializeDeserializeRoundTrip) {
    FtAllocator alloc(AnonArena());
    Bitmap bm(&alloc);
    for (uint32_t v : std::vector<uint32_t>{1, 2, 3, 65536, 999999}) bm.Add(v);

    std::vector<uint8_t> bytes = bm.Serialize();
    Bitmap restored = Bitmap::Deserialize(&alloc, bytes.data(), bytes.size());

    EXPECT_EQ(restored.Cardinality(), bm.Cardinality());
    for (uint32_t v : std::vector<uint32_t>{1, 2, 3, 65536, 999999})
        EXPECT_TRUE(restored.Contains(v));
}

TEST(BitmapTest, RecoveryFromMmapAfterReopen) {
    const std::string path = "/tmp/bitmap_recovery_test.dat";
    std::remove(path.c_str());

    uint64_t root_off = 0;
    {
        FtAllocator alloc(FileArena(path));
        Bitmap bm(&alloc);
        for (uint32_t v : std::vector<uint32_t>{7, 8, 9, 65536, 70000, 1000000})
            bm.Add(v);
        root_off = bm.root_offset();
        ASSERT_NE(root_off, 0u);
    }

    {
        FtAllocator alloc(FileArena(path));
        Bitmap bm(&alloc, root_off);
        EXPECT_EQ(bm.Cardinality(), 6u);
        for (uint32_t v : std::vector<uint32_t>{7, 8, 9, 65536, 70000, 1000000})
            EXPECT_TRUE(bm.Contains(v));
        bm.Add(42);
        EXPECT_TRUE(bm.Contains(42));
    }

    std::remove(path.c_str());
}
