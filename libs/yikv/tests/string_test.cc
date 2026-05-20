#include "container/string.h"
#include "alloc/ft_allocator.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <string>

using yikv::alloc::AllocatorOptions;
using yikv::alloc::FtAllocator;
using yikv::container::String;

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

TEST(StringTest, AssignAppendClear) {
    FtAllocator alloc(AnonArena());
    String s(&alloc);
    EXPECT_TRUE(s.empty());
    s.assign("hello");
    EXPECT_EQ(s.str(), "hello");
    s.append(" world");
    EXPECT_EQ(s.view(), "hello world");
    EXPECT_EQ(s.size(), 11u);
    EXPECT_STREQ(s.c_str(), "hello world");
    EXPECT_EQ(s[1], 'e');
    EXPECT_EQ(s.at(4), 'o');
    EXPECT_THROW(static_cast<void>(s.at(100)), std::out_of_range);
    s += "!";
    s += '?';
    EXPECT_EQ(s.str(), "hello world!?");
    EXPECT_TRUE(s == "hello world!?");
    EXPECT_NE(s, "hello");
    EXPECT_GT(s.compare("hello"), 0);
    s.clear();
    EXPECT_TRUE(s.empty());
}

TEST(StringTest, RecoveryFromMmap) {
    const std::string path = "/tmp/string_recovery_test.dat";
    std::remove(path.c_str());

    uint64_t off = 0;
    {
        FtAllocator alloc(FileArena(path));
        String s(&alloc);
        s.assign("persisted-value");
        off = s.root_offset();
    }
    {
        FtAllocator alloc(FileArena(path));
        String s(&alloc, off);
        EXPECT_EQ(s.str(), "persisted-value");
        s.append("-ok");
        EXPECT_EQ(s.str(), "persisted-value-ok");
    }

    std::remove(path.c_str());
}

TEST(StringTest, SubstrFindAndPrefixSuffix) {
    FtAllocator alloc(AnonArena());
    String s(&alloc);
    s.assign("abc-xyz-123");

    EXPECT_EQ(s.substr(0, 3), "abc");
    EXPECT_EQ(s.substr(4, 3), "xyz");
    EXPECT_EQ(s.find("xyz"), 4u);
    EXPECT_EQ(s.find("missing"), std::string::npos);
    EXPECT_TRUE(s.starts_with("abc"));
    EXPECT_FALSE(s.starts_with("xyz"));
    EXPECT_TRUE(s.ends_with("123"));
    EXPECT_FALSE(s.ends_with("abc"));
}
