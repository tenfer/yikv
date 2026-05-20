## Why

在 yikv_server 与 MinIO 已就绪、`db_path=/data/db` 的前提下，需要一条可重复的流程：在对象存储中放置与契约一致的**测试输入**（如 Parquet），经 **publishIndex** 生成并上传制品，再经 **deployIndex**（或等价 Job）将索引部署到在线服务的 `db_path`，从而消除「no tables found」的空库状态并完成端到端验证。

## What Changes

- 定义 MinIO 侧测试数据的**布局约定**（bucket、前缀、`s3://`/`s3a://` 路径与现有 `publish-spec` 示例对齐）。
- 明确 **publishIndex** 的触发方式（Kubernetes Job / `run_publish_job.py`、所需 `PUBLISH_SPEC_JSON` 与镜像 `pipeline-worker`）。
- 明确 **deploy index** 的操作路径（pipeline agent `deployIndex`、admin reload，或与现有 `verify_product_e2e.sh` 变量的衔接）。
- 文档化或脚本化**操作顺序与验收点**（对象存在性、Job 成功、`/data/db` 出现表目录、服务日志或 RPC 行为）。

## Capabilities

### New Capabilities

- `e2e-minio-publish-deploy`：覆盖在 MinIO 构造/上传测试数据、使用 publish spec 执行发布任务、将索引部署到在线 `yikv_server`（`db_path=/data/db`）的端到端行为与验收标准。

### Modified Capabilities

- （无）当前仓库 `openspec/specs/` 下尚无既有能力定义；本变更不修改已发布的产品规范文档，仅新增变更内 spec。

## Impact

- **对象存储**：MinIO（或与 S3 API 兼容的部署）、bucket 与前缀配置。
- **Kubernetes**：`pipeline-worker` Job、`yikv` 命名空间、在线 `yikv-server` Deployment 与 PVC 挂载的 `/data/db`。
- **脚本与文档**：`deploy/e2e/`、`tools/pipeline_agent/`、可能的 `reports/` 验证输出；不涉及二进制协议 **BREAKING** 变更。
