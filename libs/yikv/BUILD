package(default_visibility = ["//visibility:public"])

load("@rules_cc//cc:defs.bzl", "cc_library")
load("@rules_cc//cc:cc_shared_library.bzl", "cc_shared_library")

# 对外推荐使用本 target（白名单）；server 与各测试也可直接引用子包。
cc_library(
    name = "yikv_core",
    deps = [
        "//libs/yikv/alloc:ft_allocator",
        "//libs/yikv/db:db",
        "//libs/yikv/index:kv_index",
        "//libs/yikv/schema:schema",
    ],
)

# Convenience aliases from the workspace root (Bazel labels cannot use '/' in the
# target name, so there is no //:src/db — use //:db or //libs/yikv/db:db).
alias(
    name = "db",
    actual = "//libs/yikv/db:db",
)

alias(
    name = "db_tool",
    actual = "//libs/yikv/db:db_tool",
)

# Single shared library merging transitive //libs/yikv/** code from //libs/yikv/db:db (Bazel
# cc_shared_library; replaces scripts/bundle-libyikv.sh for many workflows).
#   bazel build //libs/yikv:libyikv
# Output: bazel-bin/libs/yikv/libyikv.so (merged transitive static .a)。
cc_shared_library(
    name = "libyikv",
    deps = ["//libs/yikv/db:db"],
    shared_lib_name = "libyikv.so",
)
