#include "container/list.h"
#include "alloc/ft_allocator.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <string>
#include <vector>

using yikv::alloc::AllocatorOptions;
using yikv::alloc::FtAllocator;
using yikv::container::List;

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

TEST(ListTest, PushFrontBackAndOrder) {
    FtAllocator alloc(AnonArena());
    List<uint64_t> l(&alloc);
    l.push_back(2);
    l.push_front(1);
    l.push_back(3);
    EXPECT_EQ(l.front(), 1u);
    EXPECT_EQ(l.back(), 3u);
    EXPECT_EQ(l.size(), 3u);

    std::vector<uint64_t> got;
    l.for_each([&](uint64_t v) { got.push_back(v); });
    EXPECT_EQ(got.size(), 3u);
    EXPECT_EQ(got[0], 1u);
    EXPECT_EQ(got[1], 2u);
    EXPECT_EQ(got[2], 3u);
}

TEST(ListTest, PopFrontBack) {
    FtAllocator alloc(AnonArena());
    List<uint64_t> l(&alloc);
    l.push_back(1);
    l.push_back(2);
    l.push_back(3);
    l.pop_front();
    l.pop_back();
    EXPECT_EQ(l.size(), 1u);
    EXPECT_EQ(l.front(), 2u);
    EXPECT_EQ(l.back(), 2u);
}

TEST(ListTest, ClearAndConstIterator) {
    FtAllocator alloc(AnonArena());
    List<uint64_t> l(&alloc);
    l.push_back(4);
    l.push_back(5);
    l.push_back(6);

    std::vector<uint64_t> got;
    for (auto it = l.cbegin(); it != l.cend(); ++it) got.push_back(*it);
    EXPECT_EQ(got.size(), 3u);
    EXPECT_EQ(got[0], 4u);
    EXPECT_EQ(got[2], 6u);

    l.clear();
    EXPECT_TRUE(l.empty());
    EXPECT_EQ(l.size(), 0u);
}

TEST(ListTest, MutableIteratorInsertErase) {
    FtAllocator alloc(AnonArena());
    List<uint64_t> l(&alloc);
    l.push_back(1);
    l.push_back(3);
    auto it = l.begin();
    ++it;
    l.insert(it, 2);

    for (auto itr = l.begin(); itr != l.end(); ++itr) {
        if (*itr == 3) *itr = 4;
    }

    std::vector<uint64_t> got;
    for (auto x : l) got.push_back(x);
    EXPECT_EQ(got.size(), 3u);
    EXPECT_EQ(got[0], 1u);
    EXPECT_EQ(got[1], 2u);
    EXPECT_EQ(got[2], 4u);

    auto e = l.begin();
    ++e;
    l.erase(e);
    got.clear();
    for (auto x : l) got.push_back(x);
    EXPECT_EQ(got.size(), 2u);
    EXPECT_EQ(got[0], 1u);
    EXPECT_EQ(got[1], 4u);
}

TEST(ListTest, RecoveryFromMmap) {
    const std::string path = "/tmp/list_recovery_test.dat";
    std::remove(path.c_str());

    uint64_t off = 0;
    {
        FtAllocator alloc(FileArena(path));
        List<uint64_t> l(&alloc);
        l.push_back(10);
        l.push_back(20);
        l.push_back(30);
        off = l.root_offset();
    }
    {
        FtAllocator alloc(FileArena(path));
        List<uint64_t> l(&alloc, off);
        EXPECT_EQ(l.size(), 3u);
        EXPECT_EQ(l.front(), 10u);
        EXPECT_EQ(l.back(), 30u);
    }

    std::remove(path.c_str());
}
