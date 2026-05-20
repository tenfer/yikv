## Context

- 在线 **`yikv_server`** 已部署，`db_path=/data/db`（PVC），启动时若目录下无表则日志为 **no tables**。
- **MinIO** 与 **`yikv-artifacts`** bucket（或与 `deploy/k8s/overlays/e2e/server-config.json` 中 `artifact_storage` 一致）已可用；集群内 S3 端点示例：`http://minio.yikv.svc.cluster.local:9000`。
- **发布路径**：`pipeline-worker` Job 入口 **`run_publish_job.py`** 读取 **`PUBLISH_SPEC_JSON`**（与 HTTP **`POST /publishIndex`** 体相同），在 Job 内执行 **`publish_index`**（构建 + push 制品）。
- **部署路径**：在线侧通过 **`POST /deployIndex`**（**`DeployIndexBody`**：`table`、`build_id` 可选等）拉取制品、切换活跃版本并 **reload**；实现位于 **`tools/pipeline_agent/pipeline_ops.py`**，通常由本机/侧车运行的 **`pipeline_agent`（uvicorn）** 暴露，或由 E2E 脚本 **`ONLINE_AGENT_URL`** 指向该服务。

## Goals / Non-Goals

**Goals:**

- 约定 MinIO 上「测试输入」的 bucket/前缀，使 **`publish-spec`** 里的 **`input`**（如 `s3://yikv-artifacts/e2e-input/<table>/`）在 Job 运行前已存在且可读。
- 串联 **上传/准备数据 → publishIndex（Job）→ 获知 `build_id` → deployIndex → 验证 `db_path/<table>`** 的可重复步骤。
- 与现有脚本对齐：**`deploy/e2e/run-publish-job.sh`**、**`deploy/scripts/submit_publish_job.py`**、**`deploy/scripts/bootstrap_minio.py`**、示例 **`deploy/e2e/examples/publish-spec.product.json`**。

**Non-Goals:**

- 不定义新的 RPC 或改写 **BREAKING** 协议。
- 不替代完整 **`verify_product_e2e.sh`** 产品矩阵；本设计聚焦「MinIO 数据 + 发布 + 部署」最小闭环。

## Decisions

1. **测试数据来源**  
   - **首选**：仓库内已有 Parquet + **`deploy/e2e/fixtures/product/schema.json`**，用 **`mc`/`aws s3 cp`/MinIO Console** 上传到与 **`publish-spec`** 中 **`input`** 一致的前缀。  
   - **备选**：使用 **`verify_product_e2e.sh`** 生成的中间产物路径（若已有自动化上传步骤则复用）。

2. **`publish-spec` 字段**  
   - **`table`**：**MUST** 与 **`DeployIndexBody.table`** 一致。  
   - **`input`**：**MUST** 为对象存储前缀 URI，且 Job 内通过 **`AWS_*`**（如 **`submit_publish_job.py` 注入的 `AWS_ENDPOINT_URL_S3`**）解析到 MinIO。  
   - **`schema_json`**：可为镜像内相对路径（**pipeline-worker** 含 **`deploy/`**）或 Job 挂载路径；示例使用 **`deploy/e2e/fixtures/product/schema.json`**。  
   - **`cleanup_build_db_after_push`**：E2E 常为 **`true`** 以省 Job 磁盘；在线 **deploy** 依赖的是已 push 的远程制品，不依赖 Job 本地 build_db。

3. **获知 `build_id` 以调用 deployIndex**  
   - **首选**：阅读 **publish Job 日志** 或 **Job 容器 stdout**（`publish_index` 返回体含 build 标识的实践以任务实现为准）。  
   - **备选**：对象存储 **`releases/<table>/`** 下列目录取字典序最大或与时间戳约定一致。  
   - **规范**：`deployIndex` **SHALL** 在 `build_id` 缺省时采用服务端「当前活跃/最新」策略时，须在运维文档中写明；否则调用方 **MUST** 传入明确 **`build_id`**。

4. **`deployIndex` 调用面**  
   - 在线 **`yikv_server`** 不直接暴露 **deployIndex**；**MUST** 通过 **`pipeline_agent`**（或等价自动化）在能访问 **`db_path`** 与 **`artifact_storage`** 的主机上执行。  
   - E2E 可选用 **`kubectl port-forward`** 将 **`ONLINE_AGENT_URL`** 指到 **`pipeline_agent` Pod** 的 **`8787`**（默认端口以 **`pipeline_agent.py`** 为准）。

## Risks / Trade-offs

- **[Risk] Job/S3 权限或 endpoint 错误导致 publish 失败** → 与 **`run-publish-job.sh`** 一致注入 **`AWS_*`**；在 Job 前用 **`mc ls`** 或等价验证 **`input`** 可读。  
- **[Risk] `build_id` 与 deploy 不一致** → 从 publish 输出显式复制 **`build_id`**；避免手写猜测。  
- **[Risk] `pipeline_agent` 未运行，无法 deploy** → 与「仅 yikv + MinIO 已启动」场景相比，deploy 阶段 **额外需要** agent 或 **`switchReloadIndex`/`admin`** 同等能力；须在任务中写清启动方式。  
- **[Trade-off] 全量 E2E 镜像构建耗资源** → **`run-publish-job.sh`** 已支持 **`BAZEL_JOBS`** 等；可文档化保守参数。

## Migration Plan

- 无存量 schema 迁移；按任务在目标集群执行一次 **bootstrap bucket → 上传数据 → publish Job → deploy → 可选 `kubectl exec` 列 `/data/db`**。  
- 回滚：删除 **`/data/db/<table>`** 下新版本目录或恢复 PVC 快照（若存在）；对象存储上制品可保留。

## Open Questions

- 仓库内是否提供 **一键将 fixture Parquet 上传到 MinIO** 的小脚本（当前可手工 **`mc cp`**）；若任务阶段实现，本设计可引用该脚本路径。  
- **`publish_index` 返回 JSON 中 `build_id` 字段名** 是否统一为单键（实现 tasks 时读代码锁定）。
