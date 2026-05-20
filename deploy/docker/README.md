# Container images（默认 `ubuntu:26.04`；可用 `UBUNTU_IMAGE` 覆盖）

同一份多阶段 [`Dockerfile`](Dockerfile) 产出三类用途的镜像：

| 用途 | Dockerfile `--target` | 默认 / 推荐 tag | 说明 |
|------|----------------------|-----------------|------|
| **1. 开发环境** | `builder-base` | `yikv-server:dev` | Bazel + GCC 14 + 编译依赖，**含 `rclone`**；挂宿主机代码后可本地测 **`tools/artifact_sync`**（S3 拉取/推送制品）。 |
| **2. 在线服务运行** | `runtime`（`docker build` 默认阶段） | `yikv-server:latest` | **`yikv_server`** + 运行库；另带 **`rclone`**、`python3-minimal` 与 **`/opt/yikv/artifact_sync/`**（与 `pipeline_agent` 同源脚本），便于在 **init 或启动脚本** 里从对象存储拉取索引后再启动进程（`YIKV_ARTIFACT_SYNC` 指向 `artifact_sync.py`）。 |
| **3. 构建索引运行** | `pipeline-worker` | `pipeline-worker:latest` | **`yikv_import_pipeline`** + Job 侧 **`tools/pipeline_agent`** 与 **`deploy/`**（`schema` 等）、**`requirements.txt`**；二进制与脚本均来自 **`builder`**。Python 层**不**打包 **`vendor/`** / **`bazel-*`**，镜像更小。 |

### 为何曾经看起来「只有 pipeline-worker 需要 rclone」？

- **`yikv_server`（C++）不实现 S3 拉索引**：只 **`mmap` 本地 `db_path`** 下表目录；配置里可选的 **`artifact_storage`** 段 **仅给 Python 工具读**，C++ 会直接忽略（见 **[`db.md`](../../db.md)**）。
- **对象存储上的制品**（按 `build_id` 发布的目录树）由 **[`tools/artifact_sync/artifact_sync.py`](../../tools/artifact_sync/artifact_sync.py)** 通过子进程调用 **`rclone`** 做 `push` / `pull` / `versions` 等；**`pipeline_agent`**、**`pipeline-worker`** 里调的也是它。
- 因此：**在线服务要从 S3 拿索引**，典型做法是 **先** 在同一镜像或 init 容器里跑 **`artifact_sync pull`（及必要时 `switch`）**，把数据落到 **`db_path` 可见的卷** 上，**再** 启动 **`yikv_server`**，或通过 **`admin_unix_socket` 发 `reload <表名>`** 指向新目录（见 **`db.md`** §3）。


**内部**先构建 **`builder`**：**`COPY`** 仓库（根 **[`.dockerignore`](../../.dockerignore)** 剔除 **`vendor/`**、**`bazel-*`** 等），再在镜像内 **`bazel build`**（在线 Bzlmod + **`third_party/tarball`** 作为 **`--distdir`**，见 **`bazel/vendor.bazelrc`**）；BuildKit **`--mount=type=cache,target=/root/.cache/bazel`** 缓存外部仓库下载。

- **`--build-arg BAZEL_VENDOR_CONFIG=cn`**（可选）：写入 **`common --config=cn`**（国内 ghproxy 下载配置等，与仓库 `.bazelrc` 对齐）。
- **`HTTP_PROXY` / `HTTPS_PROXY`**：传给 **`builder`**，GitHub/BCR 不可达时使用。

- **上下文**：`docker build …` 的上下文根必须为 **`yikv-server`** 克隆根（含 **`MODULE.bazel`**、**`libs/yikv/`**）。旧的双目录布局见 **`deploy/docker/dockerignore.repo_root`**（归档参考）。
- **不要**仅从 `deploy/docker/` 子目录 **`docker build .`**：`COPY` 会缺少 **`MODULE.bazel`** / **`libs/yikv`**。
- **`ARG UBUNTU_IMAGE=ubuntu:26.04`**：`builder-base` 安装 **`gcc-14` / `g++-14`** 并设为默认，且写入 Bazel `action_env`（与仓库根 `.bazelrc` 的 `--config=gcc14` 一致）。**Ubuntu 22.04 (jammy)** 仓库常无 `gcc-14`，请用 **24.04 / 26.04** 或自建工具链。
- **`ARG BAZEL_DOWNLOAD_PREFIX` / `BAZEL_URL`**：下载 Bazel 二进制超时见下文「GitHub 超时」。
- **`USE_BAZEL_GITHUB_MIRROR`**：可选；为 MODULE/http_archive 启用 ghproxy（**默认关闭**；公网 ghproxy 常 **502**）。优先 **`HTTP_PROXY` / `HTTPS_PROXY`**。
- **`third_party/*_stub`** 与 APT **`-dev`** 包的对照见 **[`SYSTEM_LIBS.md`](SYSTEM_LIBS.md)**。

```bash
# yikv-server 仓库根
# 或：bash deploy/docker/build.sh -t yikv-server:latest（优先 docker，否则 podman）

docker build -f deploy/docker/Dockerfile --target builder-base -t yikv-server:dev .
docker build -f deploy/docker/Dockerfile -t yikv-server:latest .
docker build -f deploy/docker/Dockerfile --target pipeline-worker -t pipeline-worker:latest .

# 仅调试「镜像内编译」时可打 builder（体积大、耗时长）
docker build -f deploy/docker/Dockerfile --target builder -t yikv-server:build .

# 可选：工具链层单独缓存（不再区分 vendor-fetch）
bash deploy/docker/build_vendor_base_image.sh
```


## 1. 开发环境镜像（`builder-base`）

不要用带整仓 `COPY` + `bazel build` 的 **`builder`** 当日常开发镜像。

应打 **`builder-base`**，把本机 **`yikv-server`** 仓库挂载到 **`/src/yikv-server`**（镜像内已与 `Dockerfile` 工作目录对齐）：

```bash
docker build -f deploy/docker/Dockerfile --target builder-base -t yikv-server:dev .

docker run --rm -it \
  -v "$(pwd):/src/yikv-server" \
  -w /src/yikv-server \
  yikv-server:dev bash
```

容器内例如：

```bash
bazel build -c opt //:yikv_server
bazel test //tests:all_tests
# 与线上一致的 S3 制品拉取（需 config 中含 artifact_storage；见 tools/artifact_sync/README.md）
python3 tools/artifact_sync/artifact_sync.py -c /path/to/config.json pull --table YOUR_TABLE --dest "$WORK/releases/YOUR_TABLE" --switch-active
bazel run //:yikv_server -- /path/in/container/config.json   # 需自备 config 或再挂卷
```

镜像内已默认 GCC 14 及 Bazel `action_env`。在**宿主机**上若也要固定 GCC 14：安装 `gcc-14`/`g++-14` 后使用 **`bazel build --config=gcc14 ...`**（见仓库根 `.bazelrc`）。

可选：`docker run` 加 `-v yikv-bazel-cache:/root/.cache/bazel` 复用分析/编译缓存（路径按习惯调整）。

---

## 2. 在线服务运行镜像（`runtime`）

```bash
docker build -f deploy/docker/Dockerfile -t yikv-server:latest .
```

默认 build 目标即为 **`runtime`**：**`yikv_server`** + 运行期 `.so`，并预装 **`rclone`**、**`python3-minimal`** 与 **`artifact_sync`**（路径 **`$YIKV_ARTIFACT_SYNC`**，默认 **`/opt/yikv/artifact_sync/artifact_sync.py`**），方便与构建索引侧**同一套**制品协议从 S3 兼容端点拉数据。

默认 **`ENTRYPOINT` 仍直接启动 `yikv_server`**。若需在容器内先到对象存储拉索引，请用 **K8s initContainer**、**自定义 `command`/`args`** 或 **wrapper**，例如（表名、`db_path`、config 路径按环境修改）：

```bash
python3 "$YIKV_ARTIFACT_SYNC" -c /etc/yikv/config.json pull --table mytable \
  --dest /data/releases/mytable --switch-active
# 将 server 的 db_path 下 mytable 链（或目录）指到 releases 后启动；若表已加载需 admin reload
exec /usr/local/bin/yikv_server /etc/yikv/config.json
```

与 **仅跑 `yikv_server`** 的最小示例：

```bash
docker run --rm -p 9000:9000 \
  -v "$PWD/config.json:/etc/yikv/config.json:ro" \
  -v yikv-data:/data \
  yikv-server:latest
```

`config.json` 里的 `db_path` 需与数据卷挂载路径一致（例如 `/data/db`）。**`artifact_storage`** 可写在同一份 JSON 里供 **`artifact_sync -c`** 使用（与 **[`tools/artifact_sync/README.md`](../../tools/artifact_sync/README.md)** 一致）。

---

## 3. 构建索引运行镜像（`pipeline-worker`）

用于 **集群内导入 + 发布索引**：镜像内含 **`yikv_import_pipeline`**、完整 **`yikv-server` 树**（`tools/pipeline_agent`、`pipeline_ops`、**`artifact_sync`**）+ **`rclone`**；**`CMD`** 默认为 **`run_publish_job.py`**。**无** HTTP 栈；与只跑 **`yikv_server`** 的 **`runtime`** 分工不同，但 **S3 制品链路共用 `artifact_sync` + `rclone`**。

```bash
docker build -f deploy/docker/Dockerfile --target pipeline-worker -t pipeline-worker:latest .
```

编排与 MinIO / Job 参数见 **`deploy/K8S_PIPELINE_MINIO.md`**；统一部署脚本可参考 **`deploy/k8s/deploy.sh`**。

---

## Docker Hub / `docker.io` 超时

构建若卡在拉取基础镜像（`registry-1.docker.io` i/o timeout），请换可访问的镜像，例如：

```bash
# 与 deploy/k8s/deploy.sh 配合时可 export UBUNTU_IMAGE
UBUNTU_IMAGE=docker.m.daocloud.io/library/ubuntu:26.04 ./deploy.sh
```

或直接 `podman build --build-arg UBUNTU_IMAGE=...`。（**22.04** 若缺少 Arrow/Parquet 或 GCC 14，优先换 **24.04/26.04** 或按 Dockerfile 注释自行补包。）

---

## GitHub 超时（下载 Bazel 卡住）

Dockerfile 使用 **`ARG BAZEL_DOWNLOAD_PREFIX`** / **`ARG BAZEL_URL`** 拼下载地址，并对 **`curl`** 加了超时与重试。

ghproxy：`BAZEL_DOWNLOAD_PREFIX` 填到 **`https://` 之前**为止（末尾带 `https://`），脚本会拼 **`github.com/bazelbuild/...`**，最终为  
`https://ghproxy.net/https://github.com/bazelbuild/bazel/releases/...`（不要重复写两段 `https://github`）。

```bash
BAZEL_DOWNLOAD_PREFIX=https://ghproxy.net/https:// \
UBUNTU_IMAGE=docker.m.daocloud.io/library/ubuntu:26.04 \
./deploy.sh
```

若代理返回 **403**，换其它镜像或自备文件：

```bash
BAZEL_URL=https://你的站点/bazel-7.4.1-linux-x86_64 ./deploy.sh
```

此后 **`RUN bazel build`** 会拉 **MODULE / http_archive**。**默认不再**全局走 ghproxy。需要外网时优先：

```bash
HTTP_PROXY=http://host:port HTTPS_PROXY=http://host:port ./deploy.sh
```

仅在确信 ghproxy 可用时：

```bash
USE_BAZEL_GITHUB_MIRROR=1 ./deploy.sh
```

仍失败时：自建工件镜像、内网 **bazel fetch** 缓存、或稳定出口。

---

## Abseil / `reflection.cc` 等 C++ 编译失败（`gcc failed: error executing CppCompile`）

若日志只有 **`gcc failed: (Exit 1)`** 而无具体报错，确认镜像构建已带 **`--verbose_failures`**（本仓库 Dockerfile 的 `bazel build` 已加）；本地可：

```bash
bazel build -c opt --verbose_failures //:yikv_server
```

| 情况 | 处理 |
|------|------|
| **内存不足**（容器/WSL OOM，`cc1plus` internal error） | 增大 Docker / WSL 内存，或 `bazel build --jobs=1`；`.bazelrc` 里 `--config=cn` 的 `local_resources` 可参考 |
| **基础镜像 / 工具链** | 默认 **`ubuntu:26.04`** + **GCC 14**。本机可用 **`--config=gcc14`** |
| **根因在 gcc 输出里** | 把 **`--verbose_failures` 展开后的 `gcc ...` 下方 stderr 最后约 40 行**贴出再对症 |

在 **builder-base** 容器内可先确认：`gcc --version`、`df -h`、`free -h`。
