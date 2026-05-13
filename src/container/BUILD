load("@rules_cc//cc:defs.bzl", "cc_library")

package(default_visibility = ["//visibility:public"])

cc_library(
    name = "bitmap",
    srcs = ["bitmap.cc"],
    hdrs = ["bitmap.h"],
    copts = ["-std=c++17"],
    deps = ["//src/alloc:ft_allocator"],
)

cc_library(
    name = "hashmap",
    srcs = ["hashmap.cc"],
    hdrs = ["hashmap.h"],
    copts = ["-std=c++17"],
    deps = ["//src/alloc:ft_allocator"],
)

cc_library(
    name = "concurrent_hashmap",
    hdrs = ["concurrent_hashmap.h"],
    copts = ["-std=c++17"],
    deps = ["//src/container:hashmap"],
)

cc_library(
    name = "string",
    srcs = ["string.cc"],
    hdrs = ["string.h"],
    copts = ["-std=c++17"],
    deps = ["//src/alloc:ft_allocator"],
)

cc_library(
    name = "vector",
    srcs = ["vector.cc"],
    hdrs = ["vector.h"],
    copts = ["-std=c++17"],
    deps = ["//src/alloc:ft_allocator"],
)

cc_library(
    name = "list",
    srcs = ["list.cc"],
    hdrs = ["list.h"],
    copts = ["-std=c++17"],
    deps = ["//src/alloc:ft_allocator"],
)
