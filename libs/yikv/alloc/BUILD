load("@rules_cc//cc:defs.bzl", "cc_library")
load("//:bazel/yikv_compat.bzl", "YIKV_COMPAT_COPTS")

package(default_visibility = ["//visibility:public"])

cc_library(
    name = "allocator",
    hdrs = ["allocator.h"],
    copts = ["-std=c++17"] + YIKV_COMPAT_COPTS,
)

cc_library(
    name = "ft_allocator",
    srcs = ["ft_allocator.cc"],
    hdrs = ["allocator.h", "ft_allocator.h"],
    copts = ["-std=c++17"] + YIKV_COMPAT_COPTS,
    deps = [":allocator"],
    linkstatic = True,
)

cc_library(
    name = "system_allocator",
    srcs = ["system_allocator.cc"],
    hdrs = ["allocator.h", "system_allocator.h"],
    copts = ["-std=c++17"] + YIKV_COMPAT_COPTS,
    deps = [":allocator"],
    linkstatic = True,
)
