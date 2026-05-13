package(default_visibility = ["//visibility:public"])

load("@rules_cc//cc:cc_shared_library.bzl", "cc_shared_library")

# Convenience aliases from the workspace root (Bazel labels cannot use '/' in the
# target name, so there is no //:src/db — use //:db or //src/db:db).
alias(
    name = "db",
    actual = "//src/db:db",
)

alias(
    name = "db_tool",
    actual = "//src/db:db_tool",
)

# Single shared library merging transitive //src/** code from //src/db:db (Bazel
# cc_shared_library; replaces scripts/bundle-libyikv.sh for many workflows).
#   bazel build //:libyikv
# Output: bazel-bin/libyikv.so (merged static .a only via make bundle-lib today).
cc_shared_library(
    name = "libyikv",
    deps = ["//src/db:db"],
    shared_lib_name = "libyikv.so",
)
