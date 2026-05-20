# MinIO 测试数据 + publishIndex + deployIndex（E2E）

在 **`yikv_server` 与 MinIO 已启动**、`db_path=/data/db`（与 **`deploy/k8s/overlays/e2e/server-config.json`** 一致）时，按本文可在对象存储放置输入、执行发布 Job、将索引部署到在线 PVC。

## 前置条件

- 集群：`yikv` 命名空间内 **`yikv-server`** Running，PVC 挂载 **`/data/db`**。
- MinIO：`server-config.json` 中 **`artifact_storage.s3_compatible`** 指向的 bucket（默认 **`yikv-artifacts`**）与 endpoint（集群内常见为 **`http://minio.yikv.svc.cluster.local:9000`**）。
- 本机上传对象存储：若使用 **Kind + `deploy/e2e/kind-config.yaml`**，MinIO S3 API 一般映射到宿主机 **`30900`**，无需 port-forward。
  也可改用 **port-forward**（示例）：

  ```bash
  kubectl port-forward -n yikv svc/minio 9000:9000
  ```

- 契约与校验：仓库内 **`deploy/e2e/fixtures/product/schema.json`** 仅含 schema；**Parquet 需自备**（先用 Tier-0 校验）：

  ```bash
  ~/.venv/bin/python deploy/e2e/scripts/verify_product_parquet.py /path/to/product.parquet
  ```

## 1. Bucket 与测试输入

- 若 bucket 不存在：可用 **`deploy/scripts/bootstrap_minio.py`**（见脚本 `--help`），或对象存储控制台 / **`mc mb`** / **`aws s3 mb`**。
- 将 Parquet 上传到与 **`deploy/e2e/examples/publish-spec.product.json`** 中 **`input`** 一致的前缀（默认 **`s3://yikv-artifacts/e2e-input/product/`**）：

  ```bash
  export S3_ENDPOINT=http://127.0.0.1:30900   # Kind 宿主机对外端口（默认）
  ./deploy/e2e/scripts/upload_product_fixture_to_minio.sh /path/to/product.parquet
  ```

### 一键：生成 10 万行 product、上传、发布、部署索引

在仓库根（需 **Docker/Podman** 可构建 **`pipeline-worker`**，**`~/.venv`** 已 `pip install -r requirements.txt`）：

```bash
# 终端 A（可选）：若无 Kind hostPort 30900，可 port-forward
# kubectl port-forward -n yikv svc/minio 9000:9000

# 终端 B：
export S3_ENDPOINT=http://127.0.0.1:30900   # Kind 默认 30900；若仅用 port-forward 到本机 9000 则改为 9000
./deploy/e2e/scripts/run_product_100k_pipeline.sh
```

脚本步骤：生成 **`~/.cache/yikv-e2e/product_100000.parquet`** → 上传 → 构建/加载 **`pipeline-worker`** → **publish Job** → 解析 **`build_id`** → **deploy Job**（挂载 PVC **`yikv-data`**）。环境变量 **`ROWS`**、`SKIP_UPLOAD` / **`SKIP_PUBLISH`** / **`SKIP_DEPLOY`**、**`BUILD_ID`** 见脚本注释。

单独生成数据（不跑集群）：

```bash
~/.venv/bin/python deploy/e2e/scripts/generate_product_parquet.py --rows 100000 --self-check -o /tmp/product100k.parquet
```

上传优先尝试 **`aws`**；否则使用 **`minio`** Python SDK（**`upload_parquet_minio.py`**，无 TTY 依赖）；再回退 **`mc`**。

## 2. publishIndex（`pipeline-worker` Job）

**`run-publish-job.sh`** 会向 Job 注入与 MinIO 兼容的 S3 环境变量（须与集群 DNS 一致）。**`yikv_import_pipeline`** 通过 Arrow S3 直接从 **`s3://`** 读 Parquet（**不再**用 rclone 先下载）；`pipeline-worker` 镜像构建时已启用 **ARROW_S3**。



- **`AWS_ENDPOINT_URL_S3=http://minio.yikv.svc.cluster.local:9000`**
- **`AWS_ACCESS_KEY_ID=minio`** / **`AWS_SECRET_ACCESS_KEY=minio12345`**（与 e2e 文档一致；生产勿照搬）

提交并等待完成：

```bash
./deploy/e2e/run-publish-job.sh \
  --spec-file deploy/e2e/examples/publish-spec.product.json \
  --wait
```

若输入已是 **单对象**（例如 **`s3://yikv-artifacts/e2e-input/product/product_100m.parquet`**），使用 **`publish-spec.product-100m.json`**（`input` 指向该对象，避免前缀下列出多余文件）：

```bash
./deploy/e2e/run-publish-job.sh \
  --spec-file deploy/e2e/examples/publish-spec.product-100m.json \
  --wait
```

**资源**：1 亿行构建耗时长、临时盘与内存占用大；提交脚本默认 **`--active-deadline-seconds 86400`**（24h）。若不够，可在命令末尾追加透传参数，例如 **`--active-deadline-seconds 172800`**。

**获取 `build_id`**：成功时 **`run_publish_job.py`** 会在 stdout 打印**一行 JSON**（含 **`build_id`**）。示例（取最新 publish Job）：

```bash
JOB=$(kubectl get jobs -n yikv -l app.kubernetes.io/name=yikv-pipeline-publish \
  --sort-by=.metadata.creationTimestamp -o jsonpath='{.items[-1].metadata.name}')
kubectl logs -n yikv "job/${JOB}" | tail -1
```

### 失败排查（2.3）

- **`PUBLISH_SPEC_JSON` / schema 路径**：`schema_json` 须在 **`pipeline-worker`** 镜像内存在（示例为 **`deploy/e2e/fixtures/product/schema.json`**）。
- **MinIO `input` 前缀为空或 403**：确认 **`upload_product_fixture_to_minio.sh`** 已上传，且 Job 内 endpoint/密钥与 MinIO 一致。
- **`run_publish_job: PUBLISH_SPEC_JSON is required`**：Job 未传入 spec（检查 **`submit_publish_job.py`** / ConfigMap）。

## 3. deployIndex（在线 `db_path`）

**`deployIndex`** 由 **`tools/pipeline_agent/pipeline_agent.py`**（FastAPI）提供，会通过 **`artifact_sync` pull** 写入 **`SERVER_DB`**（须与 **`yikv_server` 的 `db_path`** 一致）并 **reload**。

当前 **base Kustomize 未默认部署 `pipeline_agent` Pod**。任选其一：

1. **在能访问在线 `/data/db` 的环境运行 `pipeline_agent`**：配置 **`SERVER_CONFIG`**（或与 **`overlays/e2e/server-config.json`** 等价）与 **`SERVER_DB=/data/db`**、**`ADMIN_SOCKET`** 与线上 admin socket 一致；仅在本机跑 agent 通常**无法**直接写入集群 PVC。
2. **HTTP 调用**（若已暴露 agent）：

   ```bash
   export ONLINE_AGENT_URL=http://<pipeline_agent_host>:8787
   PYTHONPATH=tools/pipeline_agent python3 tools/pipeline_agent/pipeline_http_client.py \
     --table product --deploy-only --deploy-build-id '<build_id>'
   ```

3. **完整 Tier-1 + 报告**：**[README.md](./README.md)** 中的 **`verify_product_e2e.sh`** 与 **`VERIFY_TIER1_PUBLISH`** / **`VERIFY_TIER1_DEPLOY`**。
4. **集群内 deploy Job**：**`deploy/scripts/submit_deploy_job.py`** 将 PVC **`yikv-data`** 挂到 **`/data`**，运行 **`tools/pipeline_agent/run_deploy_job.py`**（**`DEPLOY_SPEC_JSON`**）。编排示例：**`deploy/e2e/scripts/run_product_100k_pipeline.sh`**。

### 验收（3.3）

在 **`yikv-server`** Pod 内：

```bash
kubectl exec -n yikv deploy/yikv-server -- ls -la /data/db/product
kubectl logs -n yikv deploy/yikv-server --tail=80
```

日志中应能看到对应表的加载 / reload，而不仅是 **no tables found**。

## 相关文件

| 路径 | 说明 |
|------|------|
| `deploy/e2e/examples/publish-spec.product.json` | Product 表示例 publish spec（`input` 为前缀） |
| `deploy/e2e/examples/publish-spec.product-100m.json` | 单对象 **`product_100m.parquet`** 的 publish spec |
| `deploy/e2e/run-publish-job.sh` | 构建 pipeline-worker、提交 Job |
| `deploy/e2e/scripts/upload_product_fixture_to_minio.sh` | 本地上传 Parquet 到 MinIO |
| `deploy/scripts/submit_deploy_job.py` | 提交 deploy Job（共享 PVC） |
| `tools/pipeline_agent/run_deploy_job.py` | deploy Job 入口 |
| `deploy/e2e/scripts/generate_product_parquet.py` | 批量生成契约一致的 product Parquet |
| `deploy/e2e/scripts/run_product_100k_pipeline.sh` | 生成 → 上传 → publish → deploy |
| `deploy/e2e/scripts/upload_parquet_minio.py` | MinIO SDK 单文件上传（无 TTY） |
