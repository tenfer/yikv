load("@rules_cc//cc:defs.bzl", "cc_library")

package(default_visibility = ["//visibility:public"])

cc_library(
    name = "allocator",
    hdrs = ["allocator.h"],
    copts = ["-std=c++17"],
)

cc_library(
    name = "ft_allocator",
    srcs = ["ft_allocator.cc"],
    hdrs = ["allocator.h", "ft_allocator.h"],
    copts = ["-std=c++17"],
    deps = [":allocator"],
)

cc_library(
    name = "system_allocator",
    srcs = ["system_allocator.cc"],
    hdrs = ["allocator.h", "system_allocator.h"],
    copts = ["-std=c++17"],
    deps = [":allocator"],
)
