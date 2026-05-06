load("@rules_cc//cc:defs.bzl", "cc_library")

package(default_visibility = ["//visibility:public"])

cc_library(
    name = "server_config",
    srcs = ["server_config.cc"],
    hdrs = ["server_config.h"],
    copts = ["-std=c++17"],
    deps = [
        "@com_github_jbeder_yaml_cpp//:yaml-cpp",
        "@com_github_google_glog//:glog",
    ],
)
