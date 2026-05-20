load("@rules_cc//cc:defs.bzl", "cc_library")
load("//:bazel/yikv_compat.bzl", "YIKV_COMPAT_COPTS")

package(default_visibility = ["//visibility:public"])

cc_library(
    name = "doc",
    srcs = ["doc.cc"],
    hdrs = ["doc.h"],
    copts = ["-std=c++17"] + YIKV_COMPAT_COPTS,
    deps = [
        "//libs/yikv/alloc:allocator",
    ],
)

cc_library(
    name = "index_base",
    srcs = ["index.cc"],
    hdrs = ["index.h"],
    copts = ["-std=c++17"] + YIKV_COMPAT_COPTS,
    deps = [
        "//libs/yikv/alloc:allocator",
        "//libs/yikv/schema:schema",
    ],
)

# KVIndex + InvertedIndex share inheritance and ConcurrentHashMap instantiations.
# Compile them in one cc_library so the implementation is not split across DSOs
# (avoids subtle ODR/layout failures under -fPIC vs multiple .so borders).
cc_library(
    name = "kv_and_inverted",
    srcs = [
        "kv_index.cc",
        "inverted_index.cc",
    ],
    hdrs = [
        "inverted_index.h",
        "kv_index.h",
    ],
    copts = ["-std=c++17"] + YIKV_COMPAT_COPTS,
    deps = [
        ":doc",
        ":index_base",
        "//libs/yikv/container:bitmap",
        "//libs/yikv/container:concurrent_hashmap",
        "//libs/yikv/schema:schema",
    ],
)

alias(
    name = "kv_index",
    actual = ":kv_and_inverted",
)

alias(
    name = "inverted_index",
    actual = ":kv_and_inverted",
)

# Convenience umbrella target for the full index layer.
cc_library(
    name = "index",
    deps = [
        ":doc",
        ":index_base",
        ":kv_and_inverted",
    ],
)
