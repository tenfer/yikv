> **设计说明**：本节与 [schema JSON](../schema/schema.h)、[`doc.h`](doc.h)、[`kv_index.h`](kv_index.h)、[`inverted_index.h`](inverted_index.h) 对齐。示例以测试中常见写法为准（见 [`tests/kv_index_test.cc`](../../tests/kv_index_test.cc)、[`tests/inverted_index_test.cc`](../../tests/inverted_index_test.cc)）。

---

## 目录

- [约束与特点](#约束与特点)
- [Schema 准备](#schema-准备)
- [Doc 使用](#doc-使用)
- [KVIndex 使用](#kvindex-使用)
- [重启恢复](#重启恢复)
- [InvertedIndex 使用](#invertedindex-使用)
- [内存与并发](#内存与并发)
- [附录：Doc 槽位与历史说明](#附录doc-槽位与历史说明)

---

## 约束与特点

1. **分配器**：索引通过 [`Allocator`](../alloc/allocator.h) 在 arena 内分配（典型 `FtAllocator`）；Doc、`ConcurrentHashMap` 节点、倒排 `Bitmap` 均在同一 arena，可 mmap 恢复。
2. **持久化**：配合文件映射 arena 时，元数据与数据可落在映射文件中。
3. **模型**：**KVIndex** 主键表为 `ConcurrentHashMap`，`next_doc_id` **原子**递增；`Upsert` / `BatchUpsert` / **Delete** 由 **`write_mx_`** 互斥，可与 **Get**、**Put**（不同主键）等并发；多线程写 arena 须 **`AllocatorMode::Concurrent`**。**InvertedIndex** 另含 **Bitmap（SWMR）**，全表多写仍需谨慎。`Get` 与 **Delete(同键)** 并发仍可能观察到瞬时不一致，需要时由应用层保证。
4. **组件**：`KVIndex`（主键 → Doc 偏移）、`InvertedIndex`（在 KV 之上维护 term → posting `Bitmap`）。

**兼容性**：`docs_hdr_off` / `posting_hdr_off` 现为 **ConcurrentHashMap 头表区域**偏移；旧版 **HashMap `HmRoot`** 偏移无法直接打开，需重建该索引目录。

---

## Schema 准备

- 使用 `yikv::schema::Schema::LoadJson`，JSON 字段名与 **`data_type`、`field_id`、`is_pk`、`is_array`、`is_index`** 等以 canonical 格式为准（勿使用本节附录中过时 JSON 示例的字段名）。
- **主键**：`pk` 指向某一字段名；该字段须为 **`Int32` / `Int64` / `String`** 之一，`KVIndex` 据此生成 map 键字符串。
- **倒排**：仅当字段在 schema 中 **`is_index: true`** 且类型为 **`String` / `Int32` / `Int64`** 时，`InvertedIndex` 会为该字段建 posting（其他类型当前 `IndexDoc` 会跳过）。

---

## Doc 使用

- **构造**：应优先使用 **`KVIndex::NewDoc()`**（或 `InvertedIndex::NewDoc()`），其按 `schema.MaxFieldId()+1` 分配槽位并分配单调 **`doc_id`**（跨重启由 `IndexHeader` 持久递增）。
- **写字段**：按类型调用 **`put_int32` / `put_int64` / `put_string` / `array_append_*`** 等（见 [`doc.h`](doc.h)）；**`field_id` 与 schema 中 `field_id` 一致**。
- **读字段**：`get_*`、`array_view_*`、`array_get_*`。
- **`Retire()`**：释放整棵 Doc（根块 + 所有变长区）；一般由 `KVIndex::Put` / `Delete` 在替换/删除旧文档时自动调用；延迟回收由分配器 **`reclaim_delay_ns`** 与 **`ReclaimExpired()`**、`publish` 内联路径驱动（详见 [`alloc/README.md`](../alloc/README.md)、[`container/USAGE.md`](../container/USAGE.md)）。

**最小示例（写入后交给 KVIndex）：**

```cpp
#include "src/index/kv_index.h"
#include "src/alloc/ft_allocator.h"
#include "src/schema/schema.h"

yikv::alloc::FtAllocator alloc;
alloc.Open(/* AllocatorOptions 见 ft_allocator */);
yikv::schema::Schema schema;
std::string err;
assert(schema.LoadJson(kYourSchemaJson, &err));

yikv::index::KVIndex idx(&alloc, &schema);

yikv::index::Doc d = idx.NewDoc();
d.put_int64(/* pk field_id */ 1, 42);
d.put_int32(2, 30);
d.put_string(3, "Alice");
idx.Put(&d);

yikv::index::Doc out;
assert(idx.Get("42", &out));
```

---

## KVIndex 使用

| API | 说明 |
|-----|------|
| `NewDoc()` | 新 Doc + 新 `doc_id` |
| `Put(Doc*)` | **仅插入**：主键尚不存在时 **`docs_->put`**（与 `Upsert` 区分） |
| `BatchPut` | 多次 `put`；大批量时周期性 **`FlushTlc` / `ReclaimExpired`** |
| `Get(pk, Doc*)` | **`docs_->get`** |
| `BatchGet` | 批量 `get` |
| `Delete(pk)` | 若存在则 **`Retire`** Doc 并 **`erase`** |
| `Publish()` | **`FlushTlc` + `ReclaimExpired()`**（可选检查点；非 SWMR 下的「发布」语义） |
| `Size()` | 当前文档条数 |
| `index_hdr_offset()` | 持久：含 `next_doc_id` 的块偏移 |
| `docs_root_offset()` | 持久：KV **`ConcurrentHashMap`** 的 **头表区域**偏移（`head_region_offset()`） |

**须持久化以供恢复的两个量**：`index_hdr_offset()` 与 `docs_root_offset()`（非 0 即已分配，写入你的表元数据）。

---

## 重启恢复

同一 allocator mmap 同一 arena 后：

```cpp
KVIndex idx(&alloc, &schema,
            saved_index_hdr_off,
            saved_docs_hdr_off);
// 继续 Put/Get
```

`saved_docs_hdr_off` 为上次 `docs_root_offset()`；对应 **`ConcurrentHashMap`** 的头表 preamble + bucket 目录，而非旧版 HashMap 的 `HmRoot`。

---

## InvertedIndex 使用

- **构造**：第四个参数为 **`posting_hdr_off`**（posting `ConcurrentHashMap` 头表区域）；新建传 `0`。
- **`Put` / `Delete`**：若存在旧文档则先 **Deindex**，再 **`KVIndex::Put` / `Delete`**，再 **Index**；posting 表更新为即时可见。
- **Posting 键**：内部为 **`std::to_string(field_id) + "#" + normalized_term`**（term 经 `Normalize` 小写；**字符串** 值经 **`Tokenize`**：连续字母数字为一词）。
- **整型字段**：term 为 **`std::to_string(数值)`**（十进制），**不**走分词。
- **查询**：
  - `Query(field_id, term, &bitmap)`：单词 posting；
  - `QueryAnd(field_id, terms)`：所有 term 的 doc_id **交集**；
  - `QueryOr(field_id, terms)`：**并集**。
- **持久化**：除 KV 两个偏移外，还需保存 **`posting_root_offset()`**。

```cpp
yikv::index::InvertedIndex inv(&alloc, &schema,
                               index_hdr, docs_hdr, posting_hdr);

yikv::index::Doc d = inv.NewDoc();
d.put_int64(pk_fid, 1);
d.put_string(body_fid, "Hello World");
inv.Put(&d);

yikv::container::Bitmap hits(alloc, 0);
if (inv.Query(body_fid, "hello", &hits)) {
    // hits 为 doc_id 的 Roaring Bitmap
}
```

---

## 内存与并发

- **KVIndex**：`Put` / `BatchPut`（建议不同主键）与 `Get` 可并发；`Upsert` / `Delete` 之间互斥，且可与 `Get` 并发（同键读写需自洽）。**`NewDoc` + 并发 `Put`** 请使用 **`AllocatorMode::Concurrent`** 的 `FtAllocator`（见 `KVIndexConcurrentTest`）。
- **InvertedIndex**：posting 侧 **`Bitmap` 仍为 SWMR**；多线程 `Put`/`Query` 见 `bitmap.h` 与字段级约束。
- **`Publish()`**：`KVIndex::Publish()` 与 `InvertedIndex::Publish()` 调用分配器 **`FlushTlc` / `ReclaimExpired()`**，用于回收与检查点；**非** HashMap 时代的 staged 发布。
- 无需对 map 单独调用已移除的 `reclaim()`。

---

## 附录：Doc 槽位与历史说明

- 槽位布局与变长编码见 [`doc.h`](doc.h) 头注释（`Slot` 的 `a` / `b` 语义）。
- 以下为 **过时字段名** 的 JSON 示意，**请勿直接作为 `LoadJson` 输入**：
   ```json
   {
    "index": "kv",
    "fields": [
        {"name": "user_id", "type": "int64", "is_array": false, "is_index": false, "is_pk": true},
       {"name": "age", "type": "int32", "is_array": false, "is_index": false},
       {"name": "clk_list", "type": "int64", "is_array": true, "is_index": false},
       {"name": "name", "type": "string", "is_array": false, "is_index": false}
    ]
   }
   ```
- 历史伪代码（`getInt32` / `putString` 等）已废弃，**请以 `doc.h` 中显式 API 为准**。
