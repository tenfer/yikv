# E2E（Kind + MinIO + Kafka）说明

本目录脚本用于在本地 kind 集群上跑通 **构建 / 发布 / 在线** 闭环。根目录 [`README.md`](../README.md) 有环境依赖总览。

## 镜像与 Kind（少用手动步骤）

- **推荐**：在仓库根执行 **`./deploy/e2e/deploy-online.sh`** 或 **`./deploy/e2e/quickstart.sh`**。`deploy/k8s/deploy.sh` 会在 **kubectl context 为 `kind-*` 且未设 `REGISTRY`** 时 **自动** `kind load docker-image`，并用 **`--name`** 匹配当前集群（如 `kind-yikv-e2e` → `yikv-e2e`），无需自己记集群名。
- **`overlays/e2e`** 使用 **`imagePullPolicy: Never`**，避免 kubelet 去拉不存在的 **`docker.io/library/yikv-server`**。
- 若不用上述脚本、手动 `kubectl apply`，仍需自己 **`kind load`**。关闭自动 load：`KIND_LOAD=0 ./deploy/k8s/deploy.sh`。

## MinIO 输入 + publish + deploy（手工闭环）

完整步骤（bucket、上传 Parquet、`run-publish-job.sh`、`build_id`、`deployIndex`、验收）见 **[MINIO_PUBLISH_DEPLOY.md](./MINIO_PUBLISH_DEPLOY.md)**。

## 在线 `yikv_server` 初始状态与「有哪些表」

`quickstart` / `deploy-online` 拉起在线服务时，**`db_path`（PVC）上通常还没有任何表子目录**，因此启动日志里可能出现 **「no tables found」** 类提示；这表示进程已就绪、**尚无 index 数据**。表与 `{db_path}/<表名>/schema.json` 等会在 **`run-publish-job.sh`**（或 agent **`deployIndex`**）完成拉取 / 部署并 **`reload`** 之后才出现。

**发现表名**：服务端只在启动时对 **`db_path` 做本地目录扫描**（`ScanAndLoad`），**不提供**「从集群或 RPC 列举当前有哪些 index / 表名」的 API。编排与 E2E **不能使用「连上 9000 端口即可发现表列表」** 的方式；表名须来自发布 spec、契约里的 `table_name`、运维配置等 **已知输入**。

## Product 闭环验证与**强制报告（Markdown）**

报告统一写在 **仓库根目录 [`reports/`](../../reports/README.md)**（`.md` + `-console.log`），必须使用 **`~/.venv/bin/python`**（`verify_product_e2e.sh` 已固定调用该解释器）。

| 层级 | 命令 | 报告（默认路径） |
|------|------|------------------|
| Tier-0 Parquet | `~/.venv/bin/python scripts/verify_product_parquet.py <file.parquet>` | `reports/product-parquet-verify-<UTC>.md` |
| 汇总（闭环入口） | `VERIFY_TIER1_PUBLISH=1 VERIFY_TIER1_DEPLOY=1 ONLINE_AGENT_URL=... scripts/verify_product_e2e.sh <file.parquet>` | `reports/product-closed-loop-<UTC>.md`、`-operations.md`、`-console.log` |

- **仅 Tier-0**：只校验 Parquet + 契约（仍属闭环中的**离线门禁**，不是零星冒烟）。
- **完整发布/在线闭环**：需加 **`VERIFY_TIER1_PUBLISH=1`**（集群 `run-publish-job.sh`），以及按需 **`VERIFY_TIER1_DEPLOY=1`** 且配置 **`ONLINE_AGENT_URL`**；报告中 **「闭环测试执行的操作」** 会写明本轮**实际执行了哪些命令**、哪些因未设变量而跳过。

报告含：运行时间、环境、**执行的操作**、结果、完整终端日志、错误与堆栈（若有）。

环境变量：**`VERIFY_PRODUCT_REPORT`**、**`PRODUCT_CLOSED_LOOP_REPORT`**；单独跑 Tier-0 时若需跳过 venv 校验：`VERIFY_SKIP_VENV_CHECK=1`（不推荐）。

### WSL / 低内存：限制镜像构建占用

`run-publish-job.sh` 会执行 **`docker build`**（内含 **Bazel 全量编译**），并行过高容易打满 CPU/内存导致 WSL 不稳定。可在跑 **`VERIFY_TIER1_PUBLISH=1`** 时**一并导出**（按机器酌情调小）：

| 环境变量 | 作用 |
|----------|------|
| **`BAZEL_JOBS`** | 传给 Docker `--build-arg`，写入 `.bazelrc` 的 `build --jobs=N`（例如 `2`） |
| **`BAZEL_LOCAL_RAM_RESOURCES`** | Bazel 本地 RAM（例如 `4096` 或 `HOST_RAM*0.35`） |
| ~~`DOCKER_BUILD_CPUS` / `DOCKER_BUILD_MEMORY`~~ | **buildx 下 `docker build` 不支持**，请用 Docker Desktop / `.wslconfig` 限制容器宿主资源。 |

示例（保守）：

```bash
export BAZEL_JOBS=2
export BAZEL_LOCAL_RAM_RESOURCES=HOST_RAM*0.35
```

另可在 Windows 用户目录 **`.wslconfig`** 里为 WSL2 配置 `memory=`、`processors=`，从虚拟机层面封顶。

### `docker build` 上下文（`.dockerignore`）

**`docker build`/BuildKit** 上下文为 **`yikv-server` 仓库根**（与 [`deploy/docker/Dockerfile`](../docker/Dockerfile) 对齐）。请在仓库根维护 **[`.dockerignore`](../../.dockerignore)** — 剥离 **`vendor/`**、`bazel-*` 等以减小上传；镜像内 **`builder`** 在线 **`bazel build`**（依赖缓存见 Dockerfile）。

| 环境变量 | 作用 |
|----------|------|
| **`BAZEL_VENDOR_CONFIG`** | 例如 **`cn`**，传给 **`--build-arg`**，镜像内写入 **`common --config=…`** |

旧的双目录 **`dockerignore.repo_root`** 仅作存档，见 **`deploy/docker/dockerignore.repo_root`**。

系统库 / 桩对照见 **`deploy/docker/SYSTEM_LIBS.md`**。可选只打 **`builder-base`**：**`deploy/docker/build_vendor_base_image.sh`**。

相关脚本：**`quickstart.sh`**、**`run-publish-job.sh`**、**`scripts/upload_product_fixture_to_minio.sh`**、指南 **`MINIO_PUBLISH_DEPLOY.md`**、示例 **`examples/publish-spec.product.json`**、契约 **`fixtures/product/schema.json`**。
