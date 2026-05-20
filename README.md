# yikv-server

[English README](README.en.md)

## 1. 项目作用

**yikv-server** 在 **`libs/yikv/`** 内嵌的 **yikv 核心 KV 引擎** 快照之上提供 **多表 KV 在线服务**：数据落在 `{db_path}/{表名}/` 的 mmap arena 与索引中，通过 RPC 暴露 **Get / Put / PutBatch / BatchGet**。

- **引擎同步**：有独立上游 `yikv` 仓库时，运行 **`scripts/sync_yikv_lib.sh`**（或 `YIKV_SOURCE=/path/to/yikv ./scripts/sync_yikv_lib.sh`）刷新 `libs/yikv/`。

- **源码布局约定**：团队对 **`libs` / 常驻服务端 / `apps/workers`（批处理）** 的路径语义见 **[`libs/yikv/README.md`](libs/yikv/README.md)** 章节 **「仓库目录布局（团队约定）」**。

- **双协议、同端口**：**brpc `baidu_std`**（`BaiduMasterService`）与 **gRPC `h2:grpc`**（[`proto/yikv_grpc.proto`](proto/yikv_grpc.proto)）；业务载荷均为 **FlatBuffers**（[`proto/yikv_server.fbs`](proto/yikv_server.fbs) → [`gen/yikv_server_generated.h`](gen/yikv_server_generated.h)）。
- **离线建索引**：对外通过 **`pipeline_agent` HTTP API** 编排导入与制品；底层由 **`yikv_import_pipeline`** 写入 `KVIndex`。**调度与业务只调 HTTP**，勿在机器上手跑导入命令。
- **实时增量（可选）**：表目录下 **`table.json`** 配置 Kafka，由 **KafkaSource** 消费 JSON 变更写入同一索引（格式见下文简述）。

更细的契约、排错与内部流程见 **[`db.md`](db.md)**。

---

## 2. 快速安装

**环境**

- **Bazel**（与仓库 [`MODULE.bazel`](MODULE.bazel) / [`.bazelversion`](.bazelversion) 一致）。
- **系统**：**OpenSSL**；编译/运行导入与压测需 **Apache Arrow C++ / Parquet**（如 `libarrow-dev`、`libparquet-dev`）。
- **MySQL 源导入（可选）**：`libmysqlclient-dev`（构建）及运行时的 **`libmysqlclient.so`**。

**构建**

```bash
cd yikv-server
bazel build //:yikv_server //:yikv_import_pipeline //:yikv_server_bench
```

可执行文件在 `bazel-bin/`（或 `bazel run //:yikv_server -- [/path/to/config.json]`，可省略参数以使用默认路径）。默认从网络/cache 解析 Bzlmod（见 [`bazel/vendor.bazelrc`](bazel/vendor.bazelrc)）。可选本地 vendor：**`bazel vendor --vendor_dir=vendor`** 生成 **`vendor/`**（含 **`VENDOR.bazel`**，仅本地、已由 **`.gitignore`** 忽略）后 **`bazel build --config=vendor`**；离线 **`--config=vendor_offline`** 仍依赖 **`third_party/tarball`** 与已填充的 **`vendor/`**。**`docker build`**：镜像内 **`builder`** 在线解析；国内可加 **`--build-arg BAZEL_VENDOR_CONFIG=cn`**。

**版本**：发布以 **语义化 git tag（如 `v0.1.0`）** 为主；[`MODULE.bazel`](MODULE.bazel) 顶层 `module(version = …)` 应在发版时与 tag **对齐**，便于下游与锁文件对照。

**外部依赖核心库**：Bazel 上对外推荐使用 **`//libs/yikv:yikv_core`**（聚合 `alloc`/`db`/`index`/`schema`）；细粒度依赖各子 target 仍以 `//libs/yikv/…` 为准。保障级别：**源码/API 兼容性 + semver**；未承诺跨编译器的 **C++ 二进制 ABI**（见 **`docs/YIKV_CORE_API.md`**）。

**配置**

复制并编辑 [`config.example.json`](config.example.json) 为 `config.json`。服务与导入工具**共用**其中的 `db_path`、`arena_seg_gb`、`arena_max_gb`、`exclusive_arena_lock`（**不可**在导入 CLI 里改这些，避免与线上一致）。

| 字段 | 说明 |
|------|------|
| `db_path` | 数据根目录；每个子目录名即表名 |
| `listen` | 监听地址，默认 `0.0.0.0:9000` |
| `arena_seg_gb` / `arena_max_gb` | 单段与总 arena 上限 (GiB) |
| `exclusive_arena_lock` | 默认 `true`，导入与服务不要同时写同一库 |
| `kafka.default_brokers` | 可选；表级可在 `table.json` 覆盖 |

**启动方式与默认值**

- **命令行**：`yikv_server [配置文件路径]` —— **可省略**第二个参数：未传参时依次尝试环境变量 **`YIKV_SERVER_CONFIG`**（非空则用）、否则 **`/etc/yikv/config.json`**（与容器内常见挂载路径一致）。显式路径：`yikv_server ./config.json`。镜像 **`ENTRYPOINT`** 仅启动二进制，不设 **`CMD`** 时即走上述默认。
- **JSON 内容**：**必填仅 `db_path`**。其余键可省略，语义与 [`apps/yikv_server/server_config.h`](apps/yikv_server/server_config.h) 中 **`ServerConfig` 默认成员**一致（与 [`config.example.json`](config.example.json) 对齐，例如 `listen`、`arena_seg_gb` / `arena_max_gb`、`exclusive_arena_lock`、`kafka.default_brokers`）。若 **`admin_unix_socket` 省略或留空**，加载逻辑会将其设为 **`{db_path 的父目录}/admin.sock`**（见 `LoadServerConfig`）。
- **每个表（index）目录**：`{db_path}/<表名>/` 内需有 **`schema.json`**（离线索引构建写入，描述列与主键；与引擎约定一致）。可选同目录 **`table.json`** 配置 Kafka 等（见上表「表级」说明与 [`apps/yikv_server/table_config.h`](apps/yikv_server/table_config.h)）。

### 2.1 容器镜像与 Kubernetes（Ubuntu 22.04）

- **镜像**：多阶段构建见 [`deploy/docker/Dockerfile`](deploy/docker/Dockerfile)。在 **`yikv-server` 仓库根**执行：

  ```bash
  docker build -f deploy/docker/Dockerfile -t yikv-server:latest .
  ```

  或：`bash deploy/docker/build.sh -t yikv-server:latest`（同上，自动选对 `docker`/`podman`）

  仅构建编译阶段镜像：`docker build -f deploy/docker/Dockerfile --target builder -t yikv-server:build .`

- **说明**：构建阶段镜像安装 `libflatbuffers-dev`、`nlohmann-json3-dev` 等，与常见开发机一致；仓库内 `third_party` 头文件占位不全时由系统头补全。运行阶段镜像仅含二进制与运行时 `.so`（如 OpenSSL、librdkafka）。**容器内开发环境**（挂宿主机源码）用 `builder-base`，见 [`deploy/docker/README.md`](deploy/docker/README.md#开发环境推荐-builder-base)。

- **K8s**：清单使用 Kustomize，[`deploy/k8s/base`](deploy/k8s/base) + [`deploy/k8s/overlays/prod`](deploy/k8s/overlays/prod)。一键脚本（需 `docker`、`kubectl`、`python3`）：

  ```bash
  ./yikv-server/deploy/k8s/deploy.sh
  REGISTRY=myregistry.example ./yikv-server/deploy/k8s/deploy.sh   # 构建、push、应用并 rollout
  KIND_LOAD=1 ./yikv-server/deploy/k8s/deploy.sh                   # kind：构建后将镜像 load 进集群
  ```

  **本机跑通示例**：安装 [kubectl](https://kubernetes.io/docs/tasks/tools/#kubectl) 与 [kind](https://kind.sigs.k8s.io/docs/user/quick-start/)，推荐 **MinIO + Kafka + 在线 + 构建 Job 闭环**：[`deploy/e2e/README.md`](deploy/e2e/README.md)（`quickstart.sh` / `bootstrap-kind.sh` / `deploy-online.sh` / `run-publish-job.sh`）。  
  最小在线：自建 `kind` 集群后 `KIND_LOAD=1 ./yikv-server/deploy/k8s/deploy.sh`（集群需有默认 `StorageClass`；缺省 overlay 不含 MinIO 与 Kafka 依赖，需自备或改用 `overlays/e2e`）。

  卸载：`./yikv-server/deploy/k8s/teardown.sh`

- **约束**：默认 **1 副本** + `ReadWriteOnce` PVC；多副本需独立数据目录/分片策略。`ScanAndLoad` 可能较慢，Deployment 已配较长 **`startupProbe`（TCP 9000）**。

---

## 3. 流程介绍

### 3.1 离线索引构建（仅 HTTP API）

**构建机 / 线上机**运行 **[`tools/pipeline_agent/pipeline_agent.py`](tools/pipeline_agent/pipeline_agent.py)**（FastAPI）。**离线构建、推送、部署只对调度侧暴露 HTTP**；agent 在进程内调用 `yikv_import_pipeline` 与 `tools/artifact_sync`。数据源形态（Parquet/CSV/目录/云 URI/MySQL 等）与导入细节见 **[`db.md`](db.md)**；**直接执行 `yikv_import_pipeline` 仅用于排错或开发**。

**客户三步（原始数据 → 构建并发布 → 线上拉取并加载）**

```mermaid
flowchart LR
  step1[Raw_Parquet_CSV]
  pub[publishIndex_build_host]
  art[Artifact_store]
  dep[deployIndex_online_host]
  step1 --> pub --> art --> dep
```

- **第 1 步**：在构建机可访问的位置准备数据（本地目录/单文件、**或** `s3://` / `oss://` / `cos://` / `obs://` / `gs://` 等，与 `yikv_import_pipeline` 一致）。
- **第 2 步**：对构建机 agent 调用 **`POST /publishIndex`**（内部顺序：删 `BUILD_DB/<表>` → 全量导入 → `push`）；可选在成功后清理构建目录（见下 **`cleanup_build_db_after_push`**）。
- **第 3 步**：对线上机 agent 调用 **`POST /deployIndex`**（`pull` + 切 `active` + 链到 `SERVER_DB` + `reload`）。

程序化调用推荐使用 **[`tools/pipeline_agent/pipeline_http_client.py`](tools/pipeline_agent/pipeline_http_client.py)** 中的 **`YikvPipelineClient`**（`publish_index` / `deploy_index`）；命令行编排仍可用 [`tools/schedule_pipeline.py`](tools/schedule_pipeline.py)。

**`buildIndex` / `publishIndex` 请求体中的 `input`**

| `input`（JSON） | agent 映射 |
|-----------------|------------|
| **字符串**，且为构建机上已存在的 **目录** | `--input_dir` |
| **字符串**，且为 **云 URI**（上述 scheme） | 单个 `--input` |
| **字符串**，且为本地 **文件** | 单个 `--input` |
| **字符串数组** | 每个元素各追加 `--input`（本地须为已存在文件，或为云 URI） |
| 仅 **`data_dir`**（与 `input` 二选一，兼容旧客户端） | `--input_dir` |

可选 **`cleanup_build_db_after_push`: true**，或环境变量 **`CLEANUP_BUILD_DB_AFTER_PUSH=1`**：在 **`/publishIndex`** 成功 push 后删除 **`BUILD_DB/<表名>`**，释放构建机磁盘。

**依赖（构建机）**

```bash
pip install -r requirements.txt   # 在 yikv-server 仓库根目录执行
cd yikv-server   # 须已 `bazel build //:yikv_import_pipeline`（§2）
python3 tools/pipeline_agent/pipeline_agent.py   # 默认 0.0.0.0:8787，HOST/PORT/WORK 等见脚本文件头
```

**HTTP 路由摘要**

| 方法 | 路径 | 作用 |
|------|------|------|
| `GET` | `/health` | 探活 |
| `POST` | `/buildIndex` | 仅本机构建（删除 `BUILD_DB/<表>` 后全量导入） |
| `POST` | `/pushIndex` | 仅推送制品 |
| `POST` | `/publishIndex` | 构建 + 推送 |
| `POST` | `/deployIndex` | 拉取、切 `active`、更新 `SERVER_DB` 链接触发 `reload`（通常跑在**线上机**） |
| `POST` | `/pullIndex` | 仅拉取（可选不切 `active`） |
| `POST` | `/switchReloadIndex` | 本地已有 release 时切 `active` 并 `reload` |

**请求示例**：`POST /publishIndex`，`Content-Type: application/json`

单目录（也可用 `data_dir` 代替 `input`）：

```json
{
  "table": "my_table",
  "input": "/data/raw/my_table",
  "schema_json": "/abs/path/schema.json"
}
```

多文件 / 多云对象（数组 → 多次 `--input`）：

```json
{
  "table": "my_table",
  "input": ["/data/p1.parquet", "s3://bucket/obj.parquet"],
  "cleanup_build_db_after_push": true
}
```

`schema_json` 可省略（使用 agent 环境默认值）。列名须与 schema 一致（不区分大小写）。

### 3.1.1 离线索引发布与制品

推送与拉取走 **`/pushIndex`**、**`/publishIndex`**、**`/deployIndex`**。**推荐**在 **`config.server.json`**（`SERVER_CONFIG`，与 `yikv_server` 同文件）中配置顶层 **`artifact_storage`**：此时 **`ARTIFACT_CONFIG`** 默认与该文件相同，`artifact_sync -c` 读嵌套段，无需单独 **`artifact-storage.json`**。若未配置 `artifact_storage`，agent 仍可使用或自动生成 **`$WORK/artifact-storage.json`**。说明见 **[`tools/artifact_sync/README.md`](tools/artifact_sync/README.md)**。

### 3.1.2 离线索引端到端流程（构建 → 制品 → 上线）

典型 **构建机 + 线上机**；**调度只发 HTTP**（可用 [`tools/schedule_pipeline.py`](tools/schedule_pipeline.py) / [`tools/pipeline_reload.py`](tools/pipeline_reload.py)）。

| 阶段 | HTTP | 说明 |
|------|------|------|
| 1. 离线导入 | **`POST /buildIndex`** 或 **`/publishIndex`** 的第一步 | 使用与线上一致的 `schema.json`；agent 每次会先删掉本机 **`BUILD_DB/<表名>`**（默认 `$WORK/build_db/<表名>`），再全量导入，避免在原目录上重复 Open 导致数据叠加。 |
| 2. 上传制品 | **`POST /pushIndex`** 或 **`/publishIndex`** 内嵌 | 将表目录打成 **`build_id`** 写入制品库。 |
| 3. 拉取并切版本 | **`POST /deployIndex`**（线上 agent） | 在 **`$WORK/releases/<表名>/<build_id>/`** 落盘，原子更新 **`active`**。 |
| 4. 服务目录 | 含于 **`/deployIndex`** | **`SERVER_DB/<表名>`** → **`releases/<表名>/active`**（须与 **`config.json`** 的 **`db_path`** 一致）。 |
| 5. 热加载 | 含于 **`/deployIndex`** / **`/switchReloadIndex`** | 经 **`admin_unix_socket`** 发送 **`reload <表名>`**（路径与 `db_path` 默认规则见 **[`db.md`](db.md)** §3）。 |

**调度客户端**：[`tools/schedule_pipeline.py`](tools/schedule_pipeline.py)（或 [`tools/pipeline_reload.py`](tools/pipeline_reload.py)）——`POST /publishIndex` + `POST /deployIndex`；**Python 集成**见 [`tools/pipeline_agent/pipeline_http_client.py`](tools/pipeline_agent/pipeline_http_client.py)。环境变量 **`BUILD_AGENT_URL`**、**`ONLINE_AGENT_URL`**、`--table`、`--data-dir` 等见 **`--help`**。

**自检**：部署后 `readlink -f $db_path/<表名>` 应指向当前 **`…/releases/<表名>/<build_id>`**；`reload` 应答为 **`ok`**（见 **[`db.md`](db.md)**）。

### 3.2 启动服务

**唯一参数**为配置文件路径：

```bash
./bazel-bin/yikv_server ./config.json
```

确保 `db_path` 存在、磁盘与 arena 配置足够。服务**启动时**扫描 `db_path` 下的表子目录并打开；运行期中新增的表目录可在落盘后发送 **`reload <表名>`**（`admin_unix_socket`）完成首次加载，已打开表的换盘同样使用该命令（详见 **[`db.md`](db.md)**）。

### 3.3 使用示例

**RPC 调用**

- 服务名：**`yikv.db.YikvDb`**；方法：**Get、Put、PutBatch、BatchGet**。
- **brpc**：`baidu_std`，请求体为 FlatBuffers 序列化后的 `GetRequest` / `PutRequest` 等（见 `yikv_server.fbs`）。
- **gRPC**：`h2:grpc`，`FbRpcRequest.payload` / `FbRpcResponse.payload` 为**同一套** FlatBuffers 字节。

任意语言：可用 **brpc C++** 或 **官方 gRPC + `yikv_grpc.proto` 生成 Stub**，按 fbs 表构造/解析 payload。双栈细节与 PutBatch 语义见 **[`db.md`](db.md)**。

**Kafka 实时写入（可选）**

在 `{db_path}/{表名}/table.json` 配置 topic 等；消息为 JSON，字段 **`_op`**（`INSERT`/`UPSERT`/`DELETE`）、**`_ts`**（毫秒）及业务字段。全局 broker 来自 `config.json` 的 `kafka.default_brokers`。完整约定见 [`apps/yikv_server/table_config.h`](apps/yikv_server/table_config.h) 与 [`db.md`](db.md)。

---

## 4. Benchmark

**`yikv_server_bench`** 对 **Get** 发压，协议与线上一致（brpc `baidu_std`）。

**必填**

- `--server HOST:PORT`（别名 `--grpc_target`）
- `--index`（或 `--table`）：须与已加载表名一致
- 键源：**`--keys_file`** 或 **`--local_parquet PATH --pk COLUMN`**

**负载**：`--requests N` 与 `--duration_sec T` 二选一；默认 `--requests 50000`。

**常用**

| 参数 | 默认 | 说明 |
|------|------|------|
| `--workers` | 8 | 并发线程 |
| `--warmup` | 32 | 预热次数 |
| `--max_keys` | 200000 | 从 Parquet 采样主键上限 |

```bash
./bazel-bin/yikv_server_bench \
  --server 127.0.0.1:9000 \
  --index my_table \
  --keys_file ./pks.txt \
  --workers 16 \
  --requests 1000000
```

标准输出 **一行 JSON**（`qps`、`latency_ms`、`phases_ms` 等）。

---

## 延伸阅读

- 协议与导入流水线架构：**[`db.md`](db.md)**
- 离线索引 HTTP agent：**[`tools/pipeline_agent/pipeline_agent.py`](tools/pipeline_agent/pipeline_agent.py)**；Python 客户端：**[`tools/pipeline_agent/pipeline_http_client.py`](tools/pipeline_agent/pipeline_http_client.py)**
- gRPC 定义：**[`proto/yikv_grpc.proto`](proto/yikv_grpc.proto)**
