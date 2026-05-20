load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_test")
load("//:bazel/yikv_compat.bzl", "YIKV_COMPAT_COPTS")

package(default_visibility = ["//visibility:public"])

# Google Benchmark: https://github.com/google/benchmark
# Run: bazel run //tests:kv_index_benchmark -- --benchmark_min_time=0.1s
# Put* benchmarks use Iterations(...) so they do not exhaust the mmap arena.
cc_binary(
    name = "kv_index_benchmark",
    srcs = ["kv_index_benchmark.cc"],
    copts = ["-std=c++17"] + YIKV_COMPAT_COPTS,
    deps = [
        "//libs/yikv/alloc:ft_allocator",
        "//libs/yikv/index:kv_index",
        "//libs/yikv/schema:schema",
        "@google_benchmark//:benchmark_main",
    ],
)

cc_binary(
    name = "hash_vs_concurrent_benchmark",
    srcs = ["hash_vs_concurrent_benchmark.cc"],
    copts = ["-std=c++17"] + YIKV_COMPAT_COPTS,
    deps = [
        "//libs/yikv/alloc:ft_allocator",
        "//libs/yikv/container:concurrent_hashmap",
        "//libs/yikv/container:hashmap",
        "@google_benchmark//:benchmark_main",
    ],
)

# DB layer: temp-dir lifecycle, CreateKVIndex / OpenIndex, cold recovery scan.
# Run: bazel run -c opt //tests:db_benchmark -- --benchmark_min_time=0.1s
cc_binary(
    name = "db_benchmark",
    srcs = ["db_benchmark.cc"],
    copts = ["-std=c++17"] + YIKV_COMPAT_COPTS,
    deps = [
        "//libs/yikv/container:bitmap",
        "//libs/yikv/db:db",
        "@google_benchmark//:benchmark_main",
    ],
)

cc_test(
    name = "ft_allocator_test",
    size = "small",
    srcs = ["ft_allocator_test.cc"],
    copts = ["-std=c++17"] + YIKV_COMPAT_COPTS,
    deps = [
        "//libs/yikv/alloc:ft_allocator",
        "//libs/yikv/alloc:system_allocator",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "db_test",
    size = "small",
    srcs = ["db_test.cc"],
    copts = ["-std=c++17"] + YIKV_COMPAT_COPTS,
    deps = [
        "//libs/yikv/db:db",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "kv_index_test",
    size = "small",
    srcs = ["kv_index_test.cc"],
    copts = ["-std=c++17"] + YIKV_COMPAT_COPTS,
    deps = [
        "//libs/yikv/alloc:ft_allocator",
        "//libs/yikv/index:kv_index",
        "//libs/yikv/schema:schema",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "inverted_index_test",
    size = "small",
    srcs = ["inverted_index_test.cc"],
    copts = ["-std=c++17"] + YIKV_COMPAT_COPTS,
    deps = [
        "//libs/yikv/alloc:ft_allocator",
        "//libs/yikv/index:inverted_index",
        "//libs/yikv/schema:schema",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "bitmap_test",
    size = "small",
    srcs = ["bitmap_test.cc"],
    copts = ["-std=c++17"] + YIKV_COMPAT_COPTS,
    deps = [
        "//libs/yikv/alloc:ft_allocator",
        "//libs/yikv/container:bitmap",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "hashmap_test",
    size = "small",
    srcs = ["hashmap_test.cc"],
    copts = ["-std=c++17"] + YIKV_COMPAT_COPTS,
    deps = [
        "//libs/yikv/alloc:ft_allocator",
        "//libs/yikv/container:hashmap",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "concurrent_hashmap_test",
    size = "small",
    srcs = ["concurrent_hashmap_test.cc"],
    copts = ["-std=c++17"] + YIKV_COMPAT_COPTS,
    deps = [
        "//libs/yikv/alloc:ft_allocator",
        "//libs/yikv/container:concurrent_hashmap",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "string_test",
    size = "small",
    srcs = ["string_test.cc"],
    copts = ["-std=c++17"] + YIKV_COMPAT_COPTS,
    deps = [
        "//libs/yikv/alloc:ft_allocator",
        "//libs/yikv/container:string",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "vector_test",
    size = "small",
    srcs = ["vector_test.cc"],
    copts = ["-std=c++17"] + YIKV_COMPAT_COPTS,
    deps = [
        "//libs/yikv/alloc:ft_allocator",
        "//libs/yikv/container:vector",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "list_test",
    size = "small",
    srcs = ["list_test.cc"],
    copts = ["-std=c++17"] + YIKV_COMPAT_COPTS,
    deps = [
        "//libs/yikv/alloc:ft_allocator",
        "//libs/yikv/container:list",
        "@googletest//:gtest_main",
    ],
)

test_suite(
    name = "all_tests",
    tests = [
        ":db_test",
        ":ft_allocator_test",
        ":kv_index_test",
        ":inverted_index_test",
        ":bitmap_test",
        ":concurrent_hashmap_test",
        ":hashmap_test",
        ":string_test",
        ":vector_test",
        ":list_test",
    ],
)
