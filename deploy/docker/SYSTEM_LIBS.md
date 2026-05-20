# System libraries vs Bazel (`builder-base` alignment)

Ubuntu base: **`ubuntu:26.04`** (`ARG UBUNTU_IMAGE`). The image **`builder-base`** in [`Dockerfile`](Dockerfile) installs **`-dev`** packages used for headers and linker flags; **`MODULE.bazel`** uses **minimal stubs** so Bazel sees the same `cc_library` names but resolves **symbols from `/usr`** at link time where noted.

## Matrix (Docker build / linux-amd64 server)

| Area | APT (builder-base) | Bazel repo / stub | Notes |
|------|--------------------|-------------------|--------|
| OpenSSL (`libssl`, `libcrypto`) | `libssl-dev` | **`new_local_repository` `@openssl` → `-lssl`/`-lcrypto`** ([`MODULE.bazel`](../../MODULE.bazel)) empty tree under [`third_party/openssl_stub`](../../third_party/openssl_stub) | Default graph links **`@openssl`** against system libs; optional **`bazel vendor`** may add a local **`vendor/VENDOR.bazel`** to skip redundant BCR trees. |
| librdkafka | `librdkafka-dev` | **`@librdkafka`** stub → `-lrdkafka` ([`MODULE.bazel`](../../MODULE.bazel)), headers under [`third_party/librdkafka_stub`](../../third_party/librdkafka_stub) | Match runtime `rdkafka` in `pipeline-worker`/`runtime`. |
| Apache Arrow / Parquet | `libarrow-dev`, `libparquet-dev` | Targets use **`linkopts = ["-larrow"]`** (and parquet where linked) against system libs ([`BUILD.bazel`](../../BUILD.bazel)); FlatBuffers headers **`libflatbuffers-dev`** | No vendored Arrow runtime in binaries from Bazel layer. |
| MySQL client C API | `libmysqlclient-dev` | **`copts`** include `-I/usr/include/mysql`, `-I/usr/include/mariadb` in indexer sources ([`BUILD.bazel`](../../BUILD.bazel)) | Runtime uses dynamically resolved **`libmysqlclient*`** names in **`pipeline-worker`**. |
| nlohmann JSON | **`nlohmann-json3-dev`** (headers on disk) | **`local_tarball` `@nlohmann_json`** from [`third_party/tarball`](../../third_party/tarball)—pinned header tree for Bazel `deps` ([`MODULE.bazel`](../../MODULE.bazel)) | Could be switched later to `-I/usr/include` + `:json` shim to drop tarball; kept for lockfile reproducibility today. |
| zlib / zstd / lz4 (runtime deps of Arrow etc.) | Indirect `-dev`; runtime stage pulls **`zlib1g`**, **`libzstd1`**, **`liblz4-1`** | Abseil / protobuf etc. compiled by Bazel | Keep Ubuntu image digest pinned when tuning prod. |

## JDK（为何看起来像「很多版本」）与「能否用系统 JDK」

`rules_java` 会向解析图注册 **多套远程 JDK + java_tools**（8/11/17/21 × 多 OS/架构）。这不是应用在运行时轮流用这么多 JVM，而是 **Bazel 可选的工具链仓库**。离线 vendor 时体积主要来自把这些树拷进 `vendor/`。

- **收窄体积**（仅在使用 **`bazel vendor`** 时）：在本地 **`vendor/VENDOR.bazel`** 里对不需要的平台/OS **`ignore()`**；Linux amd64 可只保留 **`remotejdk21_linux`** 等。
- **改用宿主 JDK**：可走 **`rules_java` 的本地运行时 / `--tool_java_runtime_version`** 等，需与 protobuf/Java 插件版本对齐并单独验证；默认远程 JDK 为的是 **可重复构建**，不依赖镜像里装了哪套 `openjdk`。

## CMake（能否用系统 `/usr/bin/cmake`）

当前 **`rules_foreign_cc`** 会从仓库拉取 **CMake 二进制 + `cmake_src`**（在线解析；不必提交 **`vendor/`**）。改用 **`apt install cmake` + 宿主二进制** 一般要配置 **`native_tools_toolchain`**（或等价机制）把 CMake 指到 **`/usr/bin/cmake`**，并保证版本满足 brpc 等下限；改动面较大。

## Policy

1. Prefer **APT + stub / `linkopts`** for C/C++ libs that Ubuntu ships at a compatible ABI.
2. Keep **large or forked sources** under Bazel (**`third_party/tarball`**, `local_tarball`) when version must match protobuf/brpc/tooling (see [`MODULE.bazel`](../../MODULE.bazel)).
3. Optional offline: after changing **`MODULE.bazel`** / lockfile, refresh local **`vendor/`**: `bazel vendor --vendor_dir=vendor`（本地维护 **`vendor/VENDOR.bazel`** 等忽略规则）。
4. For **offline** builds, `--config=vendor_offline` needs populated **`vendor/`**, **`third_party/tarball`**, and any repo left non-vendored must still be obtainable or already in dist cache.
