# ft_allocator 实现原理

`ft_allocator` 是一个基于 `mmap` 的持久化内存分配器（[`Allocator`](allocator.h) 的具体实现之一），实现目标是同时满足：

- 小对象高频分配性能（size-class + thread local cache）
- 大对象连续页分配能力（bitmap page allocator）
- 元数据可恢复（重启后可重新打开 arena）
- 延迟回收能力（时间窗 + SWMR 读侧假设；见下文）

## 整体架构

分配器可以理解为两层组合：

- 页层（Page Allocator）：使用位图管理整个 arena 的页使用情况。
- 块层（Block Allocator）：
  - 小/中对象：基于 size class 的自由链表分配。
  - 大对象：直接申请连续页。

释放策略支持两种模式：

- `Immediate`：立即回收，可被后续分配立即复用。
- `Delayed`：进入 per-class / delayed-large 链表，记录释放时刻的 **`steady_clock` 纳秒时间戳**（`free_time_ns`），在满足 **`年龄 >= AllocatorOptions::reclaim_delay_ns`** 后才可被回收；默认延迟约 5s，可按部署调小/调大。

回收触发：

- 显式调用 **`ReclaimExpired()`**（扫描 delayed 链表，回收 `free_time_ns <= now - reclaim_delay_ns` 的块）。
- **`Malloc()`** 内每 **256** 次分配节流调用一次自动回收（避免每次分配都扫链表）。

[`Allocator`](allocator.h) 接口仅暴露 **`ReclaimExpired()`**；历史 `ReclaimDelayed` 已删除。

## 内存布局

在 mmap 区域中，主要布局如下：

1. `AllocatorMetadata`（偏移 0）
2. Page bitmap
3. Data pages（实际分配区域）

其中 `AllocatorMetadata` 持久保存关键状态，例如：

- `page_size`、`page_count`、`free_page_count`
- `data_offset`、`bitmap_offset`
- `current_epoch`（仍用于发布/统计等，**不再**驱动 delayed 回收阈值）
- 各 size class 的元数据（`classes[]`）

## 核心数据结构

### BlockHeader（每块固定头）

每个块前 16 字节为 `BlockHeader`，包含：

- `magic`：块头校验，检测内存损坏或非法指针
- `class_id`：所属 size class，大对象使用 `kLargeClassId`
- `flags`：分配状态（allocated / delayed）
- `block_size`：块总大小（含 header）
- `page_count`：大对象占用页数（小对象为 0）

### FreeChain（空闲块链）

当块处于空闲状态时，其 payload 起始位置复用为 `FreeChain`：

- `next_offset`：下一空闲块偏移（delayed 链表同样使用该字段串联）
- `free_time_ns`：以 `FreeMode::Delayed` 释放时记录的 **`std::chrono::steady_clock` 纳秒**（与实现中 `steady_ns()` 一致）

这意味着 payload 区域在“已分配”和“空闲”两种状态下复用不同语义，降低额外元数据开销。

### MmapPtr（可重定位指针）

分配器提供 `MmapPtr<T>`，以 arena 偏移（`offset`）保存指针关系：

- 进程重启后，基址变化也能恢复引用关系
- 可用于持久化结构体中的内部指针关系

## 分配流程

## 1) 小/中对象分配

调用 `Malloc(size)` 后：

1. 通过 `find_class(size)` 计算 size class。
2. 先从当前线程 TLC（thread-local cache）取块：
   - 命中则无锁返回（fast path）。
   - 未命中则从 central freelist 批量补充 TLC。
3. 若 central freelist 为空，调用 `fill_class(class_id)`：
   - 向页分配器申请一批 slab 页
   - 按 class 的 `block_size` 切块
   - 将切出的块推入该 class 的 freelist
4. 返回前可能触发节流 **`maybe_reclaim_expired()`**。

## 2) 大对象分配

当 `size + header` 超过所有 size class：

1. 按页对齐计算总大小与页数
2. 调用 `alloc_pages(pages)` 在 bitmap 中找连续空闲页并标记为 used
3. 在页起始位置写入 `BlockHeader`
4. 返回 payload 地址

## 释放流程

`Free(ptr, mode)` 首先执行防御性检查：

- 从 payload 回推 `BlockHeader`
- 校验 `magic`
- 检查 `allocated` 标志，防止 double free

随后按 `mode` 分支：

### 1) Immediate

- 小对象：
  - 优先放回 TLC（无锁）
  - TLC 满时，先冲刷一半到 central freelist，再放入当前块
- 大对象：
  - 直接调用 `free_pages()` 清 bitmap，归还连续页

### 2) Delayed（基于时间戳）

- 写入当前 `free_time_ns = steady_ns()`
- 小对象挂入对应 class 的 delayed 链表（FIFO）
- 大对象挂入全局 delayed-large 链表
- 不立即复用，避免读线程在配置的时间窗内仍持有旧指针时发生悬挂访问

工程假设：读侧持有指针/快照的时长应 **小于 `reclaim_delay_ns`**；若存在可能超过该窗口的长事务，需调大延迟或使用额外的读者钉住协议（本层不强制实现）。

## 延迟回收（ReclaimExpired）

`ReclaimExpired()` 的语义是：

- 计算 `cutoff = steady_now - reclaim_delay_ns`（时钟未走满 `reclaim_delay_ns` 时可能暂不回收）
- 仅回收 **`free_time_ns <= cutoff`** 的 delayed 块
- 小对象：从 delayed 链表移回 free 链表（或等价路径）
- 大对象：从 delayed-large 链表移除后，归还页到 bitmap

## 重启与恢复

`Open()` 在检测到已有有效 metadata（magic/version 匹配）时，按“重开 arena”处理：

- 校验布局参数与当前配置一致性
- 清理当前线程残留 TLC
- 基于 bitmap 重新计算 `free_page_count`（bitmap 是页状态真源）
- **立即以最大 cutoff 扫清 delayed 队列**：上一进程已不存在，其读者假设失效，遗留 delayed 块必须在本进程开始服务前回收，避免 `free_time_ns` 与新的 `steady_clock` 原点不一致导致语义混乱

`CheckConsistency()` 会扫描 bitmap 并校验 `free_page_count`，用于在线一致性检查与故障排查。

## 并发与内存序

分配器支持两种模式：

- `AllocatorMode::Concurrent`
  - 开启互斥锁，支持多线程并发 `Malloc/Free`
- `AllocatorMode::SingleWriter`
  - 假设只有单写线程调用分配接口
  - 通过 `ConditionalLock` 省掉锁开销

锁顺序有明确约束以规避死锁：

- `page_mutex > class_mutexes[i] > large_delay_mutex`

在 SingleWriter 模式下，`PublishFence()` 提供 release fence，确保写线程构造对象后的发布顺序对读线程可见。

## 统计信息说明

`GetStats()` 输出两类统计：

- 持久元数据统计：`arena_size`、`page_count`、`free_page_count` 等
- 进程内原子计数：`used_bytes`、`allocation_count`、`free_count`、`delayed_count`

注意原子计数在每次 `Open()` 后重置，它们不是通过全盘扫描恢复的持久值。

## 总结

`ft_allocator` 通过“页位图 + size class + TLC + **按时间的**延迟回收”的组合，在同一实现中平衡了：

- 小对象吞吐
- 大对象空间管理
- 崩溃后恢复能力
- SWMR 下依赖可配置时间窗的安全回收语义

适合需要持久化内存结构、并希望控制分配性能与回收语义的系统组件。
