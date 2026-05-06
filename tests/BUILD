load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_test")

package(default_visibility = ["//visibility:public"])

# Google Benchmark: https://github.com/google/benchmark
# Run: bazel run //tests:kv_index_benchmark -- --benchmark_min_time=0.1s
# Put* benchmarks use Iterations(...) so they do not exhaust the mmap arena.
cc_binary(
    name = "kv_index_benchmark",
    srcs = ["kv_index_benchmark.cc"],
    copts = ["-std=c++17"],
    deps = [
        "//src/alloc:ft_allocator",
        "//src/index:kv_index",
        "//src/schema:schema",
        "@google_benchmark//:benchmark_main",
    ],
)

# DB layer: temp-dir lifecycle, CreateKVIndex / OpenIndex, cold recovery scan.
# Run: bazel run -c opt //tests:db_benchmark -- --benchmark_min_time=0.1s
cc_binary(
    name = "db_benchmark",
    srcs = ["db_benchmark.cc"],
    copts = ["-std=c++17"],
    deps = [
        "//src/container:bitmap",
        "//src/db:db",
        "@google_benchmark//:benchmark_main",
    ],
)

cc_test(
    name = "ft_allocator_test",
    size = "small",
    srcs = ["ft_allocator_test.cc"],
    copts = ["-std=c++17"],
    deps = [
        "//src/alloc:ft_allocator",
        "//src/alloc:system_allocator",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "db_test",
    size = "small",
    srcs = ["db_test.cc"],
    copts = ["-std=c++17"],
    deps = [
        "//src/db:db",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "kv_index_test",
    size = "small",
    srcs = ["kv_index_test.cc"],
    copts = ["-std=c++17"],
    deps = [
        "//src/alloc:ft_allocator",
        "//src/index:kv_index",
        "//src/schema:schema",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "inverted_index_test",
    size = "small",
    srcs = ["inverted_index_test.cc"],
    copts = ["-std=c++17"],
    deps = [
        "//src/alloc:ft_allocator",
        "//src/index:inverted_index",
        "//src/schema:schema",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "bitmap_test",
    size = "small",
    srcs = ["bitmap_test.cc"],
    copts = ["-std=c++17"],
    deps = [
        "//src/alloc:ft_allocator",
        "//src/container:bitmap",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "hashmap_test",
    size = "small",
    srcs = ["hashmap_test.cc"],
    copts = ["-std=c++17"],
    deps = [
        "//src/alloc:ft_allocator",
        "//src/container:hashmap",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "string_test",
    size = "small",
    srcs = ["string_test.cc"],
    copts = ["-std=c++17"],
    deps = [
        "//src/alloc:ft_allocator",
        "//src/container:string",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "vector_test",
    size = "small",
    srcs = ["vector_test.cc"],
    copts = ["-std=c++17"],
    deps = [
        "//src/alloc:ft_allocator",
        "//src/container:vector",
        "@googletest//:gtest_main",
    ],
)

cc_test(
    name = "list_test",
    size = "small",
    srcs = ["list_test.cc"],
    copts = ["-std=c++17"],
    deps = [
        "//src/alloc:ft_allocator",
        "//src/container:list",
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
        ":hashmap_test",
        ":string_test",
        ":vector_test",
        ":list_test",
    ],
)
