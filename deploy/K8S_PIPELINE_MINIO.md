# MinIO、双副本在线（隔离）、K8s Job 发布与双通道 API

本文说明两套正交的调用方式、制品存储（MinIO）、在线多副本数据语义，以及从控制面触发构建 Job 的推荐路径。

## 两条 API 边界

| 通道 | 用途 | 实现 |
|------|------|------|
| **Kubernetes API** | 创建 / 观察 **构建 Job Pod**（以及集群内其它工作负载） | Python **`kubernetes`** 客户端；示例脚本 **`deploy/scripts/submit_publish_job.py`** |
| **HTTP（`YikvPipelineClient`）** | 调用已部署的 **`pipeline_agent`**（构建机或在线机上的 Service URL） | **`tools/pipeline_agent/pipeline_http_client.py`**；仅 `urllib`，适合 CI/人工对远端 `POST /publishIndex`、`/deployIndex`、`GET /health` |

设计约定：

- **Job 内发布索引**：在 Pod 里执行 **`python3 run_publish_job.py`**，通过 **`import pipeline_ops`** 调用 **`publish_index`**，与 FastAPI 路由共用实现；**不要**在 Job 里再起本机 uvicorn 再用 `YikvPipelineClient` 打 localhost。
- **远程运维 / 另一套集群**：继续使用 **`YikvPipelineClient(BUILD_AGENT_URL, ONLINE_AGENT_URL)`**；与是否用 K8s 创建 Job **无关**。

## 组件与路径

| 组件 | 路径 |
|------|------|
| 发布核心逻辑 | `yikv-server/tools/pipeline_agent/pipeline_ops.py` |
| Job 入口 | `yikv-server/tools/pipeline_agent/run_publish_job.py` |
| HTTP agent（薄路由） | `yikv-server/tools/pipeline_agent/pipeline_agent.py` |
| 请求体模型 | `yikv-server/tools/pipeline_agent/pipeline_models.py` |
| 提交 Job | `yikv-server/deploy/scripts/submit_publish_job.py` |
| MinIO 引导 | `yikv-server/deploy/scripts/bootstrap_minio.py` + `minio-embedded.yaml`（先 **`MINIO_NAMESPACE=… ./deploy/scripts/apply_minio_embedded.sh`** 部署 YAML，占位符勿直接 `kubectl apply -f minio-embedded.yaml`） |
| 在线 2 副本（每 Pod 独立盘） | `yikv-server/deploy/k8s/overlays/online-2repl-isolated/` |

## `pipeline-worker` 镜像

在**仓库根**（含 `yikv/` 与 `yikv-server/`）构建：

```bash
docker build -f deploy/docker/Dockerfile --target pipeline-worker -t pipeline-worker:latest .
```

镜像内设置 **`YIKV_ROOT=/opt/yikv/yikv-server`**，默认 **`YIKV_IMPORT_BIN=/usr/local/bin/yikv_import_pipeline`**。Job 中请通过环境变量挂载 **`WORK`**、**`SERVER_CONFIG`**（若需）、**`ARTIFACT_CONFIG`** 或与 `SERVER_CONFIG` 合并的制品配置，使 `artifact_sync` 指向 MinIO 等后端（见 `tools/artifact_sync/README.md`）。

## 提交发布 Job（Kubernetes API）

安装依赖（控制机或带 `kubeconfig` 的 CI）：

```bash
pip install -r yikv-server/requirements.txt
```

准备 **`publish-spec.json`**：与 **`POST /publishIndex`** 相同的 JSON（字段见 `pipeline_models.PublishIndexBody`）。

```bash
python3 yikv-server/deploy/scripts/submit_publish_job.py \
  --namespace yikv \
  --image pipeline-worker:latest \
  --spec-file publish-spec.json \
  --set-env WORK=/data/yikvdb \
  --set-env ARTIFACT_CONFIG=/data/config.server.json
```

脚本向 Job 容器注入 **`PUBLISH_SPEC_JSON`**。若 JSON 很大，可改为挂载 **ConfigMap**/**Secret** 卷并在镜像入口读文件（当前脚本为内联 env，适合中小规格）。可选 **`--wait`** 轮询 Job 完成状态。

可选：在 Job 上设置 **`PIPELINE_REMOTE_HEALTH_URL`**（指向**远端**已存在的 `pipeline_agent` 或其它监控基 URL），`run_publish_job.py` 在发布成功后会对其执行一次 **`GET /health`**。

## MinIO 引导

在已存在命名空间的前提下（例如先 `kubectl apply` base 里的 `namespace.yaml`）：

```bash
pip install -r yikv-server/requirements.txt
python3 yikv-server/deploy/scripts/bootstrap_minio.py --namespace yikv --bucket yikv-artifacts
```

脚本应用内嵌清单（Deployment + Service + Secret），等待就绪后创建 bucket，并打印可合并进 **`SERVER_CONFIG` / `ARTIFACT_CONFIG`** 的 **`artifact_storage`** JSON（`provider: s3_compatible`）。生产环境请**轮换** Secret 中的口令。

**本机 / Kind（e2e）**：若 MinIO 通过 **hostPort** 暴露在宿主机，S3 API 一般为 **`http://127.0.0.1:30900`**（与 `deploy/e2e/kind-config.yaml` 一致）；Pod 与 Job 内仍用集群 DNS（例如 **`http://minio.yikv.svc.cluster.local:9000`**）。仅 `kubectl port-forward … 9000:9000` 时，本机再用 **`http://127.0.0.1:9000`**。

## 在线双副本（隔离数据）

覆盖层 **`deploy/k8s/overlays/online-2repl-isolated`** 用 **StatefulSet**（`replicas: 2`）+ **`volumeClaimTemplates`（RWO）** 替换 base 的 Deployment 与共享 PVC。语义与流量说明见同目录 **`README.md`**。

构建示例清单：

```bash
kubectl kustomize yikv-server/deploy/k8s/overlays/online-2repl-isolated
```

## 与 `deployIndex` 的配合

- **构建侧**：Job 或 HTTP **`/publishIndex`** 将索引推送到制品存储。
- **在线侧**：对 **在线集群上的 `pipeline_agent`** 调 **`POST /deployIndex`**（或通过 **`YikvPipelineClient.deploy_index`**），由在线机拉取、切换 active、软链 **`SERVER_DB`** 并 **`reload`**。

各环境的 **`WORK` / `SERVER_CONFIG` / `ARTIFACT_CONFIG`** 与 MinIO 凭据由运维按命名空间与 Secret 注入，不在此重复列举。
