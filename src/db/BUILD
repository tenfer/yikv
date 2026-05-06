load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_library")

package(default_visibility = ["//visibility:public"])

cc_binary(
    name = "db_tool",
    srcs = ["db_tool_main.cc"],
    copts = ["-std=c++17"],
    deps = [
        ":db",
        "//src/index:kv_index",
        "//src/schema:schema",
    ],
)

cc_library(
    name = "db",
    srcs = [
        "db.cc",
        "file_io.cc",
        "index_meta.cc",
    ],
    hdrs = [
        "db.h",
        "file_io.h",
        "index_meta.h",
    ],
    copts = ["-std=c++17"],
    deps = [
        "//src/alloc:ft_allocator",
        "//src/index:inverted_index",
        "//src/index:kv_index",
        "//src/schema:schema",
    ],
)
