# ConcurrentHashMap

Arena-backed hash table for **multiple concurrent readers and writers** in one process. Intended for use with `FtAllocator` in `AllocatorMode::Concurrent` (`allocator.h`).

## Layout (persistent)

- **`MxHeadTablePreamble`** at `head_region_off`: `magic` (`0x434d5848`), `version` (1), `stripe_shift`, `bucket_bits`, `entry_count` (logical size; accessed with GCC/Clang `__atomic_*` / `mx_detail` helpers for `acquire`/`release` on aligned `uint64_t`).
- **Bucket head table**: `bucket_count = 1 << bucket_bits` contiguous `uint64_t` slots (head-of-chain offsets). Each slot is read/written with the same atomic helpers (`memory_order_acquire` / `release` semantics).
- **Chains**: Same `HmBlock` / entry layout as `HashMap` (`hashmap.h`).

On reopen, pass the same `head_region_offset` and the same `stripe_shift` as used at creation; `stripe_shift` in the preamble must match (constructor validates).

## Concurrency (process-local)

Locks live only in heap-backed structures; they are not stored in the mmap.

- **`table_mtx_`**: `std::shared_mutex` — `get` / `put` / `erase` hold **shared** access; **rehash** takes **exclusive** access so no operation sees a half-switched bucket directory.
- **Stripes**: `std::shared_mutex` per stripe (`bucket_idx & (num_stripes - 1)`), `num_stripes = 1 << stripe_shift`.
  - **get** (default): `shared(table)` + `shared(stripe)`.
  - **put** / **erase**: `shared(table)` + `unique(stripe)`.
- **Lock-free `get` (optional)**: After **all** `put` / `erase` / rehash activity has quiesced, call **`enable_lock_free_reads()`**. Then **`get`** skips **`table_mtx_`** and stripe mutexes and only uses **atomic bucket heads** + chain walk (chains and table layout must remain unchanged). **`put`** / **`erase`** throw `std::logic_error` until **`disable_lock_free_reads()`**. The flag is **process-local** and **not** persisted on reopen.

## Allocation and delayed free

- All nodes are allocated with `alloc_->Malloc` (thread-safe in Concurrent mode).
- **`erase`** and **`retire_entry_blobs`** use `FreeMode::Delayed` for unreachable blocks / replaced blob storage so a reader that started a walk before unlink may still traverse freed chain nodes briefly.

**Lock-free read mode:** With `enable_lock_free_reads()`, there are **no** read-side stripe locks; delayed-free / `reclaim_delay_ns` assumptions for concurrent mutation **no longer apply** to `get` because **`put`/`erase` must not run**. If a writer violated this, readers would race on in-place list mutation (undefined behavior).

**Reader model (default locked `get`):** There is no cross-thread epoch API. Safety relies on:

1. **Stripe + table locking** — readers do not observe torn bucket heads; they may still hold pointers into a block that a writer has unlinked.
2. **`reclaim_delay_ns`** on the allocator — delayed frees must not be reclaimed until after the maximum time any thread could hold a read lock and still dereference arena pointers through a **prior** snapshot of a chain. In practice, keep `reclaim_delay_ns` comfortably above worst-case read critical section duration, or use `ReclaimExpired` only when quiescent.

For **inline** (`trivially_copyable` K/V) keys/values, `retire_entry_blobs` is a no-op; delayed free still applies to `HmBlock` chains removed by `erase` or rehash.

## `PublishFence`

Bucket heads use explicit atomics; `PublishFence` is not required on the hot path for visibility of head updates. Callers may still use it for allocator-wide ordering if needed elsewhere.

## Testing

`//tests:concurrent_hashmap_test` covers single-thread behavior, rehash, concurrent writers, concurrent readers + writers, mmap recovery, and stripe mismatch on reopen.

You can try `bazel test --compilation_mode=dbg --copt=-fsanitize=thread --linkopt=-fsanitize=thread //tests:...`; ThreadSanitizer may currently report races inside `FtAllocator`’s concurrent reclaim path (`maybe_reclaim_expired`) when multiple threads call `Malloc`, independent of the map’s stripe locks.
