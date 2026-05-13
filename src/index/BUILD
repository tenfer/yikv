load("@rules_cc//cc:defs.bzl", "cc_library")

package(default_visibility = ["//visibility:public"])

cc_library(
    name = "doc",
    srcs = ["doc.cc"],
    hdrs = ["doc.h"],
    copts = ["-std=c++17"],
    deps = [
        "//src/alloc:allocator",
    ],
)

cc_library(
    name = "index_base",
    srcs = ["index.cc"],
    hdrs = ["index.h"],
    copts = ["-std=c++17"],
    deps = [
        "//src/alloc:allocator",
        "//src/schema:schema",
    ],
)

cc_library(
    name = "kv_index",
    srcs = ["kv_index.cc"],
    hdrs = ["kv_index.h"],
    copts = ["-std=c++17"],
    deps = [
        ":doc",
        ":index_base",
        "//src/container:concurrent_hashmap",
        "//src/schema:schema",
    ],
)

cc_library(
    name = "inverted_index",
    srcs = ["inverted_index.cc"],
    hdrs = ["inverted_index.h"],
    copts = ["-std=c++17"],
    deps = [
        ":kv_index",
        "//src/container:bitmap",
        "//src/container:concurrent_hashmap",
        "//src/schema:schema",
    ],
)

# Convenience umbrella target for the full index layer.
cc_library(
    name = "index",
    deps = [
        ":inverted_index",
        ":index_base",
        ":kv_index",
    ],
)
