# 约束
1. **分配器**：索引通过 [`Allocator`](../alloc/allocator.h) 在 arena 内分配（典型 `FtAllocator`）；可 mmap 恢复。
2. **持久化**：配合文件映射 arena 时，元数据与数据可落在映射文件中。

# db
1. db是程序顶层入口，单例模式
2. 一个db有多个index，根目录db_path，每个index一个子目录，目录名是index name, 里面的文件都是基于ptallocator mmap的文件
3. index支持多种类型，目前是kv和inverted

# 测试用例
1. 创建一个DB，有一张kv index, 覆盖目前支持的所有数据类型，构造的数据量 >5GB
2. benchmark读性能
3. benchmark写性能
4. benchmark 单写多读性能

# 工具 `db_tool`

Bazel 目标：`//src/db:db_tool`。

**准备目录**（需有足够磁盘，建议arena最大虚拟地址空间 ≥ 目标数据量 + 余量）：

```bash
sudo mkdir -p /data/yikv && sudo chown "$USER" /data/yikv
```

**1. 全类型 KV 索引 + >5GB 数据**（默认 `target-gb=5.5`，可调 `--arena-seg-gb` / `--arena-max-gb`）：

```bash
bazel run //src/db:db_tool -- fill --db /data/yikv --index kv_all --target-gb 5.2 --recreate \
  --arena-seg-gb 1 --arena-max-gb 12
```

**2–4. Benchmark**（需先 `fill`）：

```bash
bazel run //src/db:db_tool -- bench-read  --db /data/yikv --index kv_all --ops 200000
bazel run //src/db:db_tool -- bench-write --db /data/yikv --index kv_all --ops 5000
bazel run //src/db:db_tool -- bench-swmr  --db /data/yikv --index kv_all --threads 8 --seconds 10
```

说明：`bench-write` 会在同库下临时创建 `kv_all_bench_wr` 索引做重写入测试，结束后删除；`bench-read` / `bench-swmr` 使用已填充的 `kv_all`。`bench-write` 与 `bench-swmr` 的写入侧使用**小行**（仍覆盖全部字段类型），以免大 `--ops` 或长时间跑满 `max_arena_size`；超大数据集请用 `fill`。
