# yikv

`yikv` is a layered retrieval and storage project under active refactoring base on mmap. **Bazel** is the primary build system.

## Project status & contact

The project is **still in progress**; APIs and on-disk formats may change. If you run into **bugs** or have questions from real use, please contact: **fansichi@qq.com**.

**Job search:** the maintainer is **looking for engineering roles** (systems / storage / infrastructure / backend—areas close to this codebase). Referrals, openings, or a short intro are welcome at the same address: **fansichi@qq.com**.

## Repository layout

- `src/alloc`: mmap / arena allocators
- `src/container`: core containers (`HashMap`, `Bitmap`, `List`, `Vector`, `String`, …); see [`src/container/README.md`](src/container/README.md) and [`src/container/USAGE.md`](src/container/USAGE.md)
- `src/schema`: unified schema and document metadata
- `src/index`: KV, inverted, and vector index interfaces and implementations

## Prerequisites

### Installing Bazel (Bazelisk)

Use **[Bazelisk](https://github.com/bazelbuild/bazelisk)** as the **`bazel`** command so [`.bazelversion`](.bazelversion) is picked up automatically. **Linux x86_64**:

```bash
sudo curl -L https://github.com/bazelbuild/bazelisk/releases/latest/download/bazelisk-linux-amd64 -o /usr/local/bin/bazel
sudo chmod +x /usr/local/bin/bazel
```

Other OS or CPU (e.g. **`bazelisk-linux-arm64`**): download the matching binary from the [Bazelisk releases](https://github.com/bazelbuild/bazelisk/releases) page and install it onto your `PATH` as `bazel` the same way.

- **Bazel** (required): builds go through **Bazel** only; the repo pins **7.4.1** in [`.bazelversion`](.bazelversion) (installed Bazelisk will download that release on first run).
- **C++17 toolchain**: GCC or Clang with `ar` / `ranlib`. `make bundle-lib` and `install` link `libyikv.so` with **`g++`** unless you set **`CXX`**.
- **Bash**: `Makefile` **`install-headers`** relies on Bash (`read -d ''`); **`SHELL := /bin/bash`** is assumed.
- **Network** (first build): Dependencies resolve via **Bazel Central Registry** and two custom **`--registry=`** sources for **babylon** / **secretflow** (see [`bazel/registries/default.bazelrc`](bazel/registries/default.bazelrc)). Default is **GitHub direct** (`raw.githubusercontent.com`). **`MODULE.bazel` tarball URLs** (**`brpc`**, **`bazel_features`**) try **GitHub Releases first**, then **`ghproxy.net`** as fallback. Inside mainland China, use **`make GHPROXY=1 …`** so Makefile (and **`scripts/bundle-libyikv.sh`** when **`YIKV_BAZEL_GHPROXY=1`** is set) load [`bazel/registries/ghproxy.bazelrc`](bazel/registries/ghproxy.bazelrc) instead of the default registry file. For **`bazel`** without Make: `bazel --noworkspace_rc --bazelrc=bazel/registries/ghproxy.bazelrc --bazelrc=bazel/buildflags.bazelrc build //…`. Plain **`bazel build`** with only the workspace [`.bazelrc`](.bazelrc) uses **direct** registries by default.

Convenience targets (`make all`, `make install`) still invoke Bazel under the hood—you need a working **`bazel`** on `PATH`.

## Build

```bash
bazel build //... && bazel test //tests:all_tests
bazel build //:libyikv    # one merged libyikv.so (cc_shared_library; same role as bundle merge)

make all          # tools + tests + lib merge → bazel-bin/libyikv.{a,so}
make GHPROXY=1 all   # optional: babylon/secretflow registry JSON via ghproxy (mainland China)
make bundle-lib   # lib merge only ([scripts/bundle-libyikv.sh](scripts/bundle-libyikv.sh))
make clean
make test
make check        # same tests, Bazel -c dbg
```

## Install

Install layout follows GNU conventions: **`$(DESTDIR)$(PREFIX)`** (default **`PREFIX=/usr/local`**). **`make install`** copies **`libyikv.so`** and **`libyikv.a`** by default (**`INSTALL_STATIC=0`** → shared only; **`INSTALL_SHARED=0`** → static archive only), **`include/yikv/`** mirrors **`src/**/*.h`** (e.g. **`include/yikv/src/db/db.h`**), **`yikv-db_tool`**, README under **`share/doc/yikv`** (skip with **`INSTALL_DOCS_CLI=0`**; skip headers with **`INSTALL_HEADERS=0`**).

```bash
sudo make install                                          # typical (.so + .a)
make install DESTDIR=/tmp/stage PREFIX=/usr              # staging for packages
sudo make install INSTALL_STATIC=0                       # shared library only
sudo make install INSTALL_SHARED=0                       # libyikv.a only
sudo make uninstall                                        # same PREFIX / DESTDIR as install
```

Portable CPU tuning when building the merged library: **`PORTABLE=1 make bundle-lib`** (generic x86-64) or **`PORTABLE=haswell`** etc. Overrides: **`BINDIR`**, **`LIBDIR`**, **`INCLUDEDIR`**, **`CXX`** (links `.so`; see **`scripts/bundle-libyikv.sh`**).

Consume installed headers as **`#include "src/db/db.h"`** with **`-I$PREFIX/include/yikv`** and **`-std=c++17 -pthread`**:

```bash
g++ -std=c++17 -pthread -I/usr/local/include/yikv app.cc \
  -L/usr/local/lib -Wl,-rpath,/usr/local/lib -lyikv
# static link: .../libyikv.a (installed by default together with libyikv.so)
```

## Notes
- Prefer extending via the storage abstraction in `src/storage/store.h` and existing Bazel targets.
- Allocators: [`src/alloc/README.md`](src/alloc/README.md). HashMap / Bitmap usage: [`src/container/USAGE.md`](src/container/USAGE.md).

---

## Database API (`yikv::db::DB`)

The `DB` class is a **process-wide singleton**. You initialize it once with `Init`, then use `Instance()` everywhere. Each logical **index** is a subdirectory of the database root and owns its own mmap arena and metadata files.

### Initialization

```cpp
#include "src/db/db.h"
#include "src/schema/schema.h"

yikv::db::DBOptions opt;
opt.db_path = "/var/lib/yikv/data";   // root directory (created if missing)
opt.alloc_defaults.arena_size = 256ull * 1024 * 1024;  // default arena segment sizing; see below
// opt.exclusive_arena_lock = true;  // default; see "Arena lock file" below

yikv::db::DB::Init(std::move(opt));
yikv::db::DB& db = yikv::db::DB::Instance();
```

- **`db_path`**: Root path for all indexes. `Init` creates this directory if it does not exist.
- **`alloc_defaults`**: Default `yikv::alloc::AllocatorOptions` for every index. The DB sets `path` per index to `<db_path>/<index_name>/arena` (and growth segments). **`mode` defaults to `AllocatorMode::Concurrent`** in `AllocatorOptions`. You normally tune **`arena_size`**, **`segment_size`**, **`max_arena_size`**, **`mode`** (set **`SingleWriter`** only for single-threaded mutation paths), and **`reclaim_delay_ns`** here; do not rely on `path` in `alloc_defaults` for multi-index layout—the DB overwrites it per index.
- **`exclusive_arena_lock`** (default **`true`**): Before mmap, each opened index acquires an advisory non-blocking **`flock(LOCK_EX)`** on `<db_path>/<name>/arena.lock` (creating the file if missing). That reduces the chance of two processes mapping the same **`MAP_SHARED`** arena for writes and corrupting it. Locks are released when the index slot is torn down (`CloseAll` / process exit). This is **advisory** (all cooperating writers must use the same locking discipline). Behavior on networked filesystems can differ from local disks. Set to **`false`** only in tightly controlled tooling or tests—not for concurrent production writers on the same index directory.

- Call **`Init` exactly once** per process. Calling `Instance()` before `Init` throws. Calling `Init` when already initialized throws.

For unit tests, **`DB::ResetForTest()`** tears down the singleton so another `Init` can run in the same process.

### On-disk layout per index

For an index named `<name>` (non-empty, no `/` or `\`):

| Path | Purpose |
|------|---------|
| `<db_path>/<name>/schema.json` | Schema JSON (written at create / used on open) |
| `<db_path>/<name>/index.meta.json` | Index kind (KV vs inverted) and recovery offsets into the arena |
| `<db_path>/<name>/arena` | Primary mmap arena file (`FtAllocator`); additional `.segN` files may appear when the arena grows |
| `<db_path>/<name>/arena.lock` | Advisory exclusive lock file (when `exclusive_arena_lock` is true); touched on first open |

### Creating indexes

Load or build a `yikv::schema::Schema`, then create either a **KV** or **inverted** index:

```cpp
yikv::schema::Schema schema;
std::string err;
if (!schema.LoadJson(json_string, &err)) { /* handle err */ }

db.CreateKVIndex("main", schema);           // exact PK lookup; HashMap → Doc offsets
db.CreateInvertedIndex("search", schema);   // KV store + term postings for `is_index` fields
```

- **`CreateKVIndex` / `CreateInvertedIndex`**: Create a **new** directory `<db_path>/<name>/`. If it already exists, creation fails.
- After creation, the index is **open** in the current process.

### Opening existing indexes

After a restart, recreate the same `DBOptions` (same `db_path` and compatible allocator defaults), call `Init`, then **open** each index you need:

```cpp
db.OpenIndex("main");   // no-op if already open
```

`OpenIndex` reads `schema.json` and `index.meta.json`, mmaps the arena, and reconstructs `KVIndex` or `InvertedIndex` from persisted offsets.

### Accessing indexes

```cpp
yikv::index::KVIndex*       kv = db.GetKVIndex("main");
yikv::index::InvertedIndex* inv = db.GetInvertedIndex("search");
```

- Wrong type (e.g. `GetKVIndex` on an inverted index) throws.
- Unknown name throws.

### Closing

```cpp
db.CloseAll();   // drops in-memory index handles and closes allocators; data on disk remains
```

Typical **recovery** pattern: `CloseAll` or process exit → later `Init` + `OpenIndex` for each index.

---

### Schema (JSON)

Schemas are loaded with `Schema::LoadJson`. Canonical JSON includes `table_name`, `pk` (primary key field name), and a `fields` array. Each field should have a stable **`field_id`**, **`data_type`** (e.g. `int32`, `int64`, `string`), **`is_pk`**, and for inverted participation **`is_index`**.

- **KV index**: Primary key field drives storage keys; other fields are stored in the arena `Doc`.
- **Inverted index**: Subclasses `KVIndex` and maintains posting lists for fields with **`is_index: true`**. Only those fields participate in `Query` / `QueryAnd` / `QueryOr`.

Compatibility rules for evolving schemas are documented in `src/schema/schema.h` (e.g. SparseRowBinary allows appending new fields with new `field_id` values).

---

### `KVIndex` usage

Header: `src/index/kv_index.h`.

1. **`NewDoc()`** — Allocates a new document with a fresh `doc_id` and slots sized from `schema.MaxFieldId()`.
2. **Fill fields** — Use `Doc` getters/setters with **`field_id`** values that match your schema (same IDs as in JSON), e.g. `doc.put_int64(fid, 42)`, `doc.put_string(fid, "alice")`.
3. **`Put(Doc* doc)`** — Upserts by primary key. The PK string is derived from the PK field: `int32`/`int64` → `std::to_string(...)`, `string` → the string value (other PK types are not supported in `ExtractPk`).
4. **`Publish()`** — Publishes staged hash-map changes so readers see updates (and for inverted indexes, postings publish too).

```cpp
yikv::index::KVIndex* idx = db.GetKVIndex("main");
yikv::index::Doc doc = idx->NewDoc();
doc.put_int64(kUserIdFid, 42);
doc.put_int32(kAgeFid, 7);
doc.put_string(kNameFid, "alice");
idx->Put(&doc);
idx->Publish();
```

**Read:**

- **`Get(std::string_view pk, Doc* out)`** — `pk` must match the string form used for storage (e.g. `"42"` for int64 `42`).
- **`BatchGet`**, **`Delete`**, **`BatchPut`** — See `kv_index.h`.

---

### `InvertedIndex` usage

Header: `src/index/inverted_index.h`. Inherits all KV operations; **`Put` / `Delete`** also maintain inverted postings for indexed fields.

**Write:** same as KV: `NewDoc`, set PK and indexed text fields, `Put`, `Publish`.

**Search:**

- **`Query(field_id, term, Bitmap* out)`** — Single normalized term; returns whether the term map existed (`bool`), and fills `out` with a posting bitmap of `doc_id` values.
- **`QueryAnd(field_id, terms)`** — Documents containing **all** terms in that field.
- **`QueryOr(field_id, terms)`** — Documents containing **any** of the terms.

Terms are produced from stored text by tokenization and normalization inside the inverted index implementation—query using the same kind of string you expect after normalization (see tests in `tests/db_test.cc`).

```cpp
yikv::index::InvertedIndex* idx = db.GetInvertedIndex("inv");
yikv::container::Bitmap bm(idx->alloc(), 0);
if (idx->Query(kBioFid, "hello", &bm)) {
  if (bm.Contains(doc_id)) { /* ... */ }
}
```

---

### End-to-end recovery example

Matches the flow in `tests/db_test.cc`:

1. `CreateKVIndex` / create docs / `Put` / `Publish`.
2. `CloseAll()`, then `ResetForTest()` **only in tests**, or simply exit the process.
3. New process: `Init` with the **same** `db_path`, `OpenIndex("main")`, `GetKVIndex`, `Get` by PK string.

---

### Error handling

Invalid index names, missing directories, schema/meta parse errors, and type mismatches surface as **`std::runtime_error`** or **`std::invalid_argument`** with message prefixes such as `DB::CreateKVIndex:` / `DB::OpenIndex:`. Plan to catch and log these at application boundaries.

---

## Benchmarks

Micro-benchmarks use [Google Benchmark](https://github.com/google/benchmark). Targets are plain **`cc_binary`** in [`tests/BUILD`](tests/BUILD) (not part of **`//tests:all_tests`**).

### How to run

```bash
# Build only
make benchmark
bazel build -c opt //tests:kv_index_benchmark //tests:db_benchmark

# KV index layer (mmap arena + HashMap, no DB singleton)
bazel run -c opt //tests:kv_index_benchmark -- --benchmark_min_time=0.1s

# DB layer (temp dirs, mmap, Create/Open, inverted)
bazel run -c opt //tests:db_benchmark -- --benchmark_min_time=0.1s
```

Useful flags (after **`--`**):

```bash
bazel run -c opt //tests:db_benchmark -- --benchmark_list_tests
bazel run -c opt //tests:kv_index_benchmark -- --benchmark_filter='GetHit'
```

Binaries appear under **`bazel-bin/tests/kv_index_benchmark`** and **`bazel-bin/tests/db_benchmark`**. On mainland China, use **`make GHPROXY=1 benchmark`** like other Makefile targets.

### What each binary measures

| Binary | Focus |
|--------|--------|
| **`kv_index_benchmark`** | **`KVIndex`** only: unique inserts (`PutUnique`), hot-row `Get`, scaled `Get` after N rows, same-key upserts. Arena sizing is fixed in source; several cases use **`Iterations(...)`** so mmap use stays bounded. |
| **`db_benchmark`** | Full **`DB`**: **`CreateKVIndex`** with unique PK + **`Publish`**, inverted **`Put` + `Query`**, and **cold** **`OpenIndex` + full `Get` scan** after on-disk seed rows (`BM_DB_ColdOpenScan` **Arg** = row count). |

### Reference performance (sample only)

Recorded with **`-c opt`**, Google Benchmark **`--benchmark_min_time=0.05s`**, **2026-05-06**, on **16 logical CPUs** (~3.8 GHz nominal), Linux. **Figures vary** with CPU, turbo, **`/tmp`**, and load.

#### `kv_index_benchmark`

| Benchmark | CPU time | Notes |
|-----------|----------|--------|
| `BM_KVIndex_PutUnique/512` (40k iterations) | ~4.4 µs / op | Arg 512 = arena sizing in benchmark; unique PK each op. |
| `BM_KVIndex_GetHit` | ~0.030 µs / op | Single hot key lookup. |
| `BM_KVIndex_GetHitAtScale/100` … `/10000` | ~0.026–0.027 µs / op | Same lookup after 100 / 1k / 10k seeded rows (this machine). |
| `BM_KVIndex_PutUpsertSameKey` (40k iters) | ~2.0 µs / op | Same PK every iteration. |

#### `db_benchmark`

| Benchmark | CPU time | Notes |
|-----------|----------|--------|
| `BM_DB_KV_PutUnique` (30k iters) | ~5.4 µs / iter | **`Put` + `Publish`** per iteration. |
| `BM_DB_Inverted_PutAndQuery` (15k iters) | ~5.0 µs / iter | One inverted row + **`Query("beta")`** per iter. |
| `BM_DB_ColdOpenScan/200` | ~122 µs / iter | Per iter: **`Init` → `OpenIndex` → 200 × `Get`**. |
| `BM_DB_ColdOpenScan/2000` | ~287 µs / iter | Same with 2000 keys scanned. |

Reproduce:

```bash
bazel run -c opt //tests:kv_index_benchmark -- --benchmark_min_time=0.1s --benchmark_repetitions=1
bazel run -c opt //tests:db_benchmark -- --benchmark_min_time=0.1s --benchmark_repetitions=1
```
