#include "container/vector.h"
#include "alloc/ft_allocator.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <string>

using yikv::alloc::AllocatorOptions;
using yikv::alloc::FtAllocator;
using yikv::container::Vector;

static AllocatorOptions AnonArena(std::size_t size = 64 * 1024 * 1024) {
    AllocatorOptions opts;
    opts.arena_size = size;
    return opts;
}

static AllocatorOptions FileArena(const std::string& path,
                                  std::size_t size = 64 * 1024 * 1024) {
    AllocatorOptions opts;
    opts.path = path;
    opts.arena_size = size;
    return opts;
}

TEST(VectorTest, PushAndIndex) {
    FtAllocator alloc(AnonArena());
    Vector<uint64_t> v(&alloc);
    for (uint64_t i = 0; i < 100; ++i) v.push_back(i * 2);
    EXPECT_EQ(v.size(), 100u);
    EXPECT_EQ(v[0], 0u);
    EXPECT_EQ(v[99], 198u);
}

TEST(VectorTest, PopBack) {
    FtAllocator alloc(AnonArena());
    Vector<uint64_t> v(&alloc);
    v.push_back(1);
    v.push_back(2);
    v.pop_back();
    EXPECT_EQ(v.size(), 1u);
    EXPECT_EQ(v[0], 1u);
}

TEST(VectorTest, ReserveResizeAndAt) {
    FtAllocator alloc(AnonArena());
    Vector<uint64_t> v(&alloc);
    v.reserve(64);
    EXPECT_GE(v.capacity(), 64u);
    v.resize(10, 7);
    EXPECT_EQ(v.size(), 10u);
    EXPECT_EQ(v.front(), 7u);
    EXPECT_EQ(v.back(), 7u);
    EXPECT_EQ(v.at(5), 7u);
    v[5] = 42;
    EXPECT_EQ(v.at(5), 42u);
    EXPECT_THROW(static_cast<void>(v.at(100)), std::out_of_range);
}

TEST(VectorTest, IteratorTraversal) {
    FtAllocator alloc(AnonArena());
    Vector<uint64_t> v(&alloc);
    for (uint64_t i = 1; i <= 5; ++i) v.push_back(i);
    uint64_t sum = 0;
    for (auto x : v) sum += x;
    EXPECT_EQ(sum, 15u);
}

TEST(VectorTest, InsertEraseAndEmplaceBack) {
    FtAllocator alloc(AnonArena());
    Vector<uint64_t> v(&alloc);
    v.push_back(1);
    v.push_back(3);
    v.emplace_back(4);
    v.insert(v.cbegin() + 1, 2);
    EXPECT_EQ(v.size(), 4u);
    EXPECT_EQ(v[0], 1u);
    EXPECT_EQ(v[1], 2u);
    EXPECT_EQ(v[2], 3u);
    EXPECT_EQ(v[3], 4u);

    v.erase(v.cbegin() + 2);
    EXPECT_EQ(v.size(), 3u);
    EXPECT_EQ(v[0], 1u);
    EXPECT_EQ(v[1], 2u);
    EXPECT_EQ(v[2], 4u);
}

TEST(VectorTest, RecoveryFromMmap) {
    const std::string path = "/tmp/vector_recovery_test.dat";
    std::remove(path.c_str());

    uint64_t off = 0;
    {
        FtAllocator alloc(FileArena(path));
        Vector<uint64_t> v(&alloc);
        for (uint64_t i = 0; i < 32; ++i) v.push_back(i + 10);
        off = v.root_offset();
    }
    {
        FtAllocator alloc(FileArena(path));
        Vector<uint64_t> v(&alloc, off);
        EXPECT_EQ(v.size(), 32u);
        EXPECT_EQ(v[0], 10u);
        EXPECT_EQ(v[31], 41u);
    }

    std::remove(path.c_str());
}
