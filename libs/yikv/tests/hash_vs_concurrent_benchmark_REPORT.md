# HashMap vs ConcurrentHashMap vs std::unordered_map 压测报告

## 元数据

| 项 | 值 |
|----|-----|
| 生成时间 (UTC) | 2026-05-13T05:31:28+00:00 |
| 源码 | `libs/yikv/tests/hash_vs_concurrent_benchmark.cc` |
| 构建 | `bazel build -c opt //libs/yikv/tests:hash_vs_concurrent_benchmark` |
| 原始日志 | 运行时重定向到本地（不入库）；见文末 `tee` 示例 |
| CHM 版本 | v2（5 项并发优化，见下文） |

## 环境

16 × 3864.97 MHz CPU；L1 48 KiB×8，L2 2048 KiB×8，L3 516096 KiB×1。

## ConcurrentHashMap v2 优化说明

| 优化 | 内容 |
|------|------|
| **① Seqlock** | 用 `atomic<uint32_t> table_seq_` 代替 `shared_mutex table_mtx_`；偶数=稳定，奇数=rehash。热路径从一次 `shared_mutex::lock_shared`（~12 ns）降到一次 atomic load（~2 ns）。 |
| **② stripe → `std::mutex`** | 每条 stripe 改用 `std::mutex`，无竞争成本约为 `shared_mutex` 的 1/2。并发读需求使用 `enable_lock_free_reads()`。 |
| **③ `lock_free_reads_` relaxed** | put/erase 中将 acquire load 改为 relaxed；后续 mutex acquire 已提供足够屏障。 |
| **④ per-stripe StripeCount** | `alignas(64)` 的 `atomic<int64_t>` 数组，`needs_rehash()` 和 `size()` 读进程内存而非 mmap 地址，消除跨核 cache-line bounce。preamble `entry_count` 仍逐次更新用于持久化恢复。 |
| **⑤ needs_rehash 降频** | `thread_local` 计数，每 64 次 put 才调用 `needs_rehash()` 一次（后者需对所有 stripe 求和）。 |

## 性能对比（本次运行）

### 单线程写入

| 用例 | items/s |
|------|---------|
| HashMap SingleWriterPut | ~0.54M |
| **CHM SingleWriterPut（优化前）** | **~29M** |
| **CHM SingleWriterPut（优化后）** | **~96M** |
| Std unordered_map | ~379M |

优化后 CHM 与 STL 的差距从 **13×** 缩小到约 **4×**。

### 多线程写入

| 线程数 | CHM | STL mutex |
|-------|-----|-----------|
| 2 | ~60M/s | ~68M/s |
| 4 | ~61M/s | ~68M/s |
| 8 | ~61M/s | ~67M/s |
| 16 | ~41M/s | ~41M/s |
| 32 | ~43M/s | ~43M/s |

8 线程以内 CHM 与 STL 几乎持平（CHM 使用 mmap + arena，STL 使用堆）。

### 多读（预填后纯读）

| 用例 | 1T | 2T | 4T | 8T |
|------|----|----|----|----|
| HashMap snapshot | ~73M | ~72M | ~70M | ~64M |
| **CHM LockFree** | ~74M | ~74M | ~67M | ~46M |
| **CHM 锁路径（优化后）** | ~37M | ~36M | ~37M | ~30M |
| STL shared_lock | ~40M | ~39M | ~38M | ~38M |

CHM 默认锁路径（优化②：mutex 独占）退出了并发读竞争；如需并行读，启用 `enable_lock_free_reads()` 即可达到 HashMap 快照水平（~74M/s）。

### 混合读写（1 Writer + 4 Readers，per-op 加锁）

| 用例 | mixed_writes | mixed_reads |
|------|-------------|-------------|
| HashMap 1W4R | ~512k/s | ~2.05M/s |
| **CHM 1W4R（优化前）** | ~6.2M/s | ~25.0M/s |
| **CHM 1W4R（优化后）** | **~11.2M/s** | **~44.8M/s** |
| STL 1W4R | ~11.8M/s | ~47.3M/s |
| CHM 2W4R | ~19.2M/s | ~38.3M/s |
| CHM 4W4R | ~25.9M/s | ~25.9M/s |

优化后 **CHM 1W4R 与 STL 仅差 ~5%**（而前者使用 mmap arena + 持久化路径）。

## std::unordered_map 对照说明

- STL 使用**进程堆 + `reserve`**，CHM 使用 **mmap arena**；单线程写仍有约 4× 差距，主要来自 arena allocator vs 堆分配器。
- `StdUnorderedMap_ParallelWritersMutex`：全局一把 `std::mutex` 串行化每次写；与 CHM **条带锁**不完全等价。
- `StdUnorderedMap_MultiRead_SharedLock`：每次 `find` 前 `shared_lock`，多读者并行；CHM 锁路径改用 `mutex` 后读并行度降低，并发读应启用 lock-free 路径。
- `StdUnorderedMap_1Writer4Readers_Mixed`：全局 `shared_mutex`，每 put/find 单独加锁。

## 剩余差距来源（单线程写 ~4×）

1. **mmap arena Malloc vs 堆 `new`**：每次新键需从 arena 分配 HmBlock（112 字节），比堆慢 2–3×；是最大的结构性差距。
2. **链式桶 vs 开放寻址**：HmBlock 链每跳一次可能触发 cache miss；STL 用开放寻址 + 连续内存。
3. **preamble entry_count 持久化写**：每次 put 写 mmap 地址，STL 无此开销。

如要进一步收窄差距，需重新设计为开放寻址 + arena 连续平坦桶数组（大型结构改动）。

## Google Benchmark 原始输出（全文）

```
2026-05-13T05:31:28+00:00
Running bazel-bin/tests/hash_vs_concurrent_benchmark
Run on (16 X 3864.97 MHz CPU s)
CPU Caches:
  L1 Data 48 KiB (x8)
  L1 Instruction 64 KiB (x8)
  L2 Unified 2048 KiB (x8)
  L3 Unified 516096 KiB (x1)
Load Average: 0.68, 0.46, 0.28
---------------------------------------------------------------------------------------------------------------------
Benchmark                                                           Time             CPU   Iterations UserCounters...
---------------------------------------------------------------------------------------------------------------------
BM_HashMap_SingleWriterPut                                       1.86 us         1.86 us       107021 items_per_second=537.059k/s
BM_ConcurrentHashMap_SingleWriterPut                            0.010 us        0.010 us     12674897 items_per_second=96.2657M/s
BM_StdUnorderedMap_SingleThreadPut                              0.003 us        0.003 us     62035450 items_per_second=378.785M/s
BM_ConcurrentHashMap_2WritersPut/threads:2                      0.017 us        0.017 us      9257018 items_per_second=60.4927M/s
BM_ConcurrentHashMap_4WritersPut/threads:4                      0.016 us        0.016 us      9815036 items_per_second=60.63M/s
BM_ConcurrentHashMap_ParallelWritersPut/threads:8               0.016 us        0.016 us      8075920 items_per_second=60.9457M/s
BM_ConcurrentHashMap_ParallelWritersPut/threads:16              0.024 us        0.024 us      6392032 items_per_second=41.1182M/s
BM_ConcurrentHashMap_ParallelWritersPut/threads:32              0.023 us        0.023 us      7099232 items_per_second=43.0459M/s
BM_ConcurrentHashMap_ParallelWritersPut_Stripe8/threads:32      0.025 us        0.024 us      6951968 items_per_second=41.6983M/s
BM_StdUnorderedMap_ParallelWritersMutex/threads:2               0.015 us        0.015 us     11133412 items_per_second=68.31M/s
BM_StdUnorderedMap_ParallelWritersMutex/threads:4               0.015 us        0.015 us     11258172 items_per_second=68.1087M/s
BM_StdUnorderedMap_ParallelWritersMutex/threads:8               0.015 us        0.015 us     10217520 items_per_second=67.2082M/s
BM_StdUnorderedMap_ParallelWritersMutex/threads:16              0.025 us        0.024 us      6973232 items_per_second=41.0579M/s
BM_StdUnorderedMap_ParallelWritersMutex/threads:32              0.029 us        0.023 us      7312992 items_per_second=43.4927M/s
BM_HashMap_MultiRead/threads:1                                  0.014 us        0.014 us     10449767 items_per_second=72.793M/s
BM_HashMap_MultiRead/threads:2                                  0.014 us        0.014 us     10808776 items_per_second=72.4854M/s
BM_HashMap_MultiRead/threads:4                                  0.014 us        0.014 us     11125780 items_per_second=69.92M/s
BM_HashMap_MultiRead/threads:8                                  0.016 us        0.016 us     10034568 items_per_second=64.0993M/s
BM_ConcurrentHashMap_MultiRead/threads:1                        0.027 us        0.027 us      6138952 items_per_second=36.5686M/s
BM_ConcurrentHashMap_MultiRead/threads:2                        0.028 us        0.028 us      6033870 items_per_second=35.8128M/s
BM_ConcurrentHashMap_MultiRead/threads:4                        0.027 us        0.027 us      5034916 items_per_second=36.5059M/s
BM_ConcurrentHashMap_MultiRead/threads:8                        0.033 us        0.033 us      3743784 items_per_second=30.437M/s
BM_ConcurrentHashMap_MultiRead_LockFree/threads:1               0.013 us        0.013 us     12639007 items_per_second=74.091M/s
BM_ConcurrentHashMap_MultiRead_LockFree/threads:2               0.013 us        0.013 us     12264342 items_per_second=74.2249M/s
BM_ConcurrentHashMap_MultiRead_LockFree/threads:4               0.015 us        0.015 us     11725436 items_per_second=66.9363M/s
BM_ConcurrentHashMap_MultiRead_LockFree/threads:8               0.022 us        0.022 us      6635992 items_per_second=45.9355M/s
BM_StdUnorderedMap_MultiRead_SharedLock/threads:1               0.025 us        0.025 us      6757755 items_per_second=39.6243M/s
BM_StdUnorderedMap_MultiRead_SharedLock/threads:2               0.026 us        0.026 us      6508672 items_per_second=38.8938M/s
BM_StdUnorderedMap_MultiRead_SharedLock/threads:4               0.026 us        0.026 us      6598492 items_per_second=38.4419M/s
BM_StdUnorderedMap_MultiRead_SharedLock/threads:8               0.026 us        0.026 us      6524248 items_per_second=38.4142M/s
BM_HashMap_1Writer4Readers_Mixed/iterations:2000/threads:5       25.0 us         25.0 us        10000 mixed_reads=2.04755M/s mixed_writes=511.887k/s
BM_ConcurrentHashMap_1Writer4Readers_Mixed/threads:5             1.14 us         1.14 us       141580 mixed_reads=44.7531M/s mixed_writes=11.1883M/s
BM_StdUnorderedMap_1Writer4Readers_Mixed/threads:5               1.08 us         1.08 us       150365 mixed_reads=47.2881M/s mixed_writes=11.822M/s
BM_ConcurrentHashMap_2Writers4Readers_Mixed/threads:6            1.11 us         1.11 us       143166 mixed_reads=38.3075M/s mixed_writes=19.1538M/s
BM_ConcurrentHashMap_4Writers4Readers_Mixed/threads:8            1.23 us         1.23 us       151624 mixed_reads=25.9437M/s mixed_writes=25.9437M/s
```

## 复现

```bash
cd yikv-server   # 仓库根目录
bazel run -c opt //libs/yikv/tests:hash_vs_concurrent_benchmark -- --benchmark_min_time=0.12s \
  | tee /tmp/hash_vs_concurrent_benchmark_LAST_RUN.txt
```

仅 CHM 相关：
```bash
bazel-bin/libs/yikv/tests/hash_vs_concurrent_benchmark \
  --benchmark_filter='ConcurrentHashMap' --benchmark_min_time=0.12s
```
