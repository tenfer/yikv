load("@rules_cc//cc:defs.bzl", "cc_library")

package(default_visibility = ["//visibility:public"])

exports_files(["bazel_features_module_dot_bazel_version.patch"])

# ─── Apache Arrow / Parquet（系统安装） ──────────────────────────────────────
# 安装：apt install libarrow-dev libparquet-dev
# 头文件由 C++ 工具链 system_include_directories（/usr/include）提供。

cc_library(
    name = "arrow",
    linkopts = ["-larrow"],
)

cc_library(
    name = "parquet",
    linkopts = ["-lparquet"],
    deps = [":arrow"],
)
