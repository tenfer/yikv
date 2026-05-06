# CLAUDE.md

This repository uses Bazel as the source of truth for builds and tests.

## Build Commands

```bash
./build.sh                  # build server/client/test target
./build.sh server           # build yikv_server
./build.sh client           # build yikv_client
./build.sh test             # run arch smoke test
./build.sh debug            # debug build
./build.sh clean            # bazel clean

bazel build //...
bazel test //tests:all_tests
```

## Current Architecture Notes

- Keep layering aligned with `alloc -> container -> schema/storage/index -> engine -> service`.
- `KVIndex` is backed by `container::HashMap` and stores **primary-key string to arena offset of [`index::Doc`](src/index/doc.h)** (`Doc` header + slots + var-length payloads in the same allocator).
- `FieldDef.is_index` only gates inverted index materialization when `IndexType` is `Inverted`.
- RocksDB/offline SST toolchain references are obsolete and should not be reintroduced.
