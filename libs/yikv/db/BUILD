load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_library")
load("//:bazel/yikv_compat.bzl", "YIKV_COMPAT_COPTS")

package(default_visibility = ["//visibility:public"])

cc_binary(
    name = "db_tool",
    srcs = ["db_tool_main.cc"],
    copts = ["-std=c++17"] + YIKV_COMPAT_COPTS,
    deps = [
        ":db",
        "//libs/yikv/index:kv_index",
        "//libs/yikv/schema:schema",
    ],
)

cc_library(
    name = "db",
    srcs = [
        "arena_lock.cc",
        "db.cc",
        "file_io.cc",
        "index_meta.cc",
    ],
    hdrs = [
        "arena_lock.h",
        "db.h",
        "file_io.h",
        "index_meta.h",
    ],
    copts = ["-std=c++17"] + YIKV_COMPAT_COPTS,
    deps = [
        "//libs/yikv/alloc:ft_allocator",
        "//libs/yikv/index:inverted_index",
        "//libs/yikv/index:kv_index",
        "//libs/yikv/schema:schema",
    ],
)
