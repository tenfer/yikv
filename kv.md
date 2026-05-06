# yikv
这是个单机版的KV数据库

# 数据模型
Schema化Table化，字段支持基本类型：整型/浮点型/字符串/字符串，复杂类型：数组，数组的元素只能是基本类型
这个数据库可以是有多张表。
表分按天更新的离线表和 实时更新（单写）的实时表。

# 存储
当前代码基线已移除 RocksDB 及离线 SST 工具链，统一通过 `src/storage/store.h` 抽象对接底层存储。

- 目前默认实现是 `MemoryStore`（用于开发与架构演进）。
- 索引层按统一分层建设：KV / 倒排 / 向量。
- KV 索引基于 `container::HashMap`，值为 **[`Doc`](src/index/doc.h) 在 arena 中的根偏移**（`pk_string -> slot_offset`），与 mmap 持久化方向一致。

后续如果引入磁盘型存储实现，应优先通过 `IStore` 扩展，并保持与 `schema`、`engine`、`service` 的接口兼容。






