# container 使用文档

本文介绍 `HashMap` 与 `Bitmap`（roaring bitmap）的典型使用方式，重点覆盖：
- 首次创建
- 重启恢复
- 单写多读（SWMR）下的读写流程
- 内存回收和注意事项

---

## 1. 共同前提

这两个容器通过 [`yikv::alloc::Allocator`](../alloc/allocator.h) 获取内存（典型实现是 `FtAllocator`，也可以是测试用的 `SystemAllocator`）：
- 持久化数据都在 allocator arena 中（文件映射或匿名 mmap，取决于 allocator 配置）
- 容器对象可通过 offset 恢复
- 适用场景是单写多读（SWMR）

使用前你需要先创建或恢复 allocator（示例中用 `alloc` 表示，类型为 `FtAllocator` 或任意 `Allocator*`）。

---

## 2. HashMap 使用

头文件：`container/hashmap.h`

### 2.1 首次创建

```cpp
#include "container/hashmap.h"

using Map = yikv::container::HashMap<std::string, uint64_t>;

Map kv(&alloc, /*hdr_off=*/0, /*bucket_bits=*/12);
uint64_t kv_hdr_off = kv.root_offset();  // 需要持久保存
```

说明：
- `hdr_off == 0` 表示新建
- `root_offset()` 返回稳定 header offset（不是临时 root）
- `kv_hdr_off` 需要放到你的上层元数据中，供重启恢复

### 2.2 重启恢复

```cpp
Map kv(&alloc, kv_hdr_off);
```

说明：
- 只要 mmap 到同一份 arena 文件，并拿到之前保存的 `kv_hdr_off`，即可恢复

### 2.3 写入与发布（单写线程）

```cpp
kv.put("a", 1);
kv.put("b", 2);
kv.erase("a");

// 一次性发布当前批次变更
kv.publish();
```

关键点：
- `put/erase` 只修改 writer 侧 staged 版本
- 调用 `publish()` 后，新版本才对 reader 可见
- **`publish()` 内会**：把本次 CoW 退休的节点记入时间戳队列，并按 `alloc->ReclaimDelayNs()` 内联回收已到期批次，并调用 **`alloc->ReclaimExpired()`** 清理分配器侧的 `Delayed` 块。无需再调用已移除的 `reclaim()` API

### 2.4 读取（多读线程）

```cpp
auto snap = kv.acquire_snapshot();

uint64_t v = 0;
if (snap.get("b", v)) {
    // use v
}

bool ok = snap.contains("a");
size_t n = snap.size();
```

关键点：
- 读线程应基于 `Snapshot` 读取，以获得一致视图
- `Snapshot` 是 lock-free 读句柄，适合 SWMR

### 2.5 内存回收与时间窗（HashMap）

- **CoW 旧节点**：在每次 `publish()` 末尾按 **`Allocator::ReclaimDelayNs()`**（来自 `AllocatorOptions::reclaim_delay_ns`，默认约 5s）判断是否可 `Immediate` 释放。
- **分配器 `FreeMode::Delayed`**：例如上层 `Doc` 退休的变长字段，由 **`ReclaimExpired()`** 按同一时间窗回收；`Malloc()` 还会每 **256** 次分配节流触发一次 `ReclaimExpired()`。
- **工程假设**：读线程持有 `Snapshot` 或 arena 指针的时间应 **短于** `reclaim_delay_ns`。若存在超长读，需调大该选项或引入读者钉住协议；时间窗是近似安全模型，不是形式化 RCU epoch。

测试或极短读场景可将 `reclaim_delay_ns` 设为 `0`，使延迟块尽快符合回收条件。

### 2.6 HashMap 常见建议

- 一个 map 只允许一个 writer 线程执行 `put/erase/publish`
- 对读线程，优先“拿 snapshot 后连续读”，避免每次查都重复 acquire
- 写路径无需手动 `reclaim()`；高写入时仍应周期性有 **`Malloc`** 或 **`publish`**，以便触发分配器节流回收与 HashMap 内联回收

---

## 3. Bitmap 使用

头文件：`container/bitmap.h`

### 3.1 首次创建

```cpp
#include "container/bitmap.h"

yikv::container::Bitmap bm(&alloc, /*root_off=*/0);
uint64_t bm_root_off = bm.root_offset();  // 需要持久保存
```

说明：
- `root_off == 0` 表示新建 bitmap root
- `root_offset()` 需要保存到上层元数据中

### 3.2 重启恢复

```cpp
yikv::container::Bitmap bm(&alloc, bm_root_off);
```

### 3.3 基本操作

```cpp
bm.Add(10);
bm.Add(20);
bm.Remove(10);

bool has20 = bm.Contains(20);
uint64_t card = bm.Cardinality();
bool empty = bm.IsEmpty();
```

实现语义：
- `Add/Remove` 是就地修改
- 每次 mutating 操作结束内部都会 `PublishFence()`

### 3.4 批量写入

```cpp
std::vector<uint32_t> vals = {1, 2, 2, 3, 100000};
std::sort(vals.begin(), vals.end());
bm.BulkAdd(vals.data(), vals.size());
```

关键点：
- `BulkAdd` 要求输入升序
- 重复值会自动去重

### 3.5 集合运算

```cpp
yikv::container::Bitmap a(&alloc, 0);
yikv::container::Bitmap b(&alloc, 0);

a.Add(1); a.Add(2);
b.Add(2); b.Add(3);

auto u = a.Or(b);       // 返回新 bitmap
auto i = a.And(b);      // 返回新 bitmap
auto x = a.Xor(b);      // 返回新 bitmap
auto d = a.AndNot(b);   // 返回新 bitmap

a.OrWith(b);            // 原地修改 a
a.AndWith(b);           // 原地修改 a
```

### 3.6 遍历

```cpp
bm.ForEach([&](uint32_t v) {
    // use v
});
```

### 3.7 可选导入导出

```cpp
auto bytes = bm.Serialize();
auto restored = yikv::container::Bitmap::Deserialize(&alloc, bytes.data(), bytes.size());
```

说明：
- 这是 portable wire format，适合迁移或跨实例传输
- 常规本地恢复优先使用 `root_offset()` + mmap reopen

---

## 4. 推荐使用模式（简版）

1. 启动时恢复 allocator
2. 用保存的 offset 恢复 `HashMap/Bitmap`（没有则新建）
3. 单写线程执行写入
4. HashMap 写完一批调用 `publish()`（内含回收一轮）
5. 读线程通过 `Snapshot`（HashMap）或只读接口（Bitmap）读取
6. 根据读延迟合理设置 **`AllocatorOptions::reclaim_delay_ns`**

---

## 5. 常见坑

- 忘记持久化 `root_offset()` 导致重启后无法恢复对象
- HashMap 写完不 `publish()`，读线程看不到最新数据
- **`reclaim_delay_ns` 过小** 且存在超长读，可能 UAF；过大则 arena 占用偏高
- 长期无任何分配且无 `publish`，分配器节流回收几乎不跑（仍有 `publish` 路径上的 `ReclaimExpired`）
- `BulkAdd` 输入未排序，导致性能或行为不符合预期
- 误把容器当多写并发结构使用（当前设计是 SWMR）
