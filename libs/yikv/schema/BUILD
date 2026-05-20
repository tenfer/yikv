load("@rules_cc//cc:defs.bzl", "cc_library")
load("//:bazel/yikv_compat.bzl", "YIKV_COMPAT_COPTS")

package(default_visibility = ["//visibility:public"])

cc_library(
    name = "schema",
    srcs = ["schema.cc"],
    hdrs = ["schema.h"],
    copts = ["-std=c++17"] + YIKV_COMPAT_COPTS,
    deps = [],
)
