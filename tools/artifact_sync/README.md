# Artifact 发布与同步（配置驱动）

本目录实现离线索引导出后的 **JSON 配置** 发布：`artifact_storage.provider` 为 `local`（本机目录）或 `s3_compatible`（OSS / OBS / COS / S3 / MinIO 等 S3 兼容端点），**不改变** `yikv_import_pipeline` 与 `yikv_server` 二进制。

**推荐统一配置**：把顶层 **`artifact_storage`** 写在 **`yikv_server` 使用的同一份 JSON**（线上/构建常为 **`$WORK/config.server.json`**，即环境变量 **`SERVER_CONFIG`**）里；C++ 会忽略该键，**`artifact_sync.py -c <该路径>`** 只读取其中 `artifact_storage`。**`pipeline_agent`** 在检测到该文件含 `artifact_storage` 且未设置 **`ARTIFACT_CONFIG`** 时，会把 **`ARTIFACT_CONFIG`** 默认指到同一文件，无需单独的 `artifact-storage.json`。若未在 server JSON 里配置制品段，则仍使用（或自动生成）**`$WORK/artifact-storage.json`**。

`-c` 指向的任意 JSON 均须含顶层 **`artifact_storage`** 键。

**不在对象存储里维护 `current.json`。** 「最新版」= 该表前缀下 **子目录名的字典序最大者**（因此请使用 **`YYYYMMDDHHmmss`** 这类可排序的 `build_id`，`push` 默认即生成此类 ID）。

## 文件

- 制品字段 **`artifact_storage`** 的形态见仓库根 [`config.example.json`](../../config.example.json)。**推荐**直接写进 **`config.server.json`** 与 `yikv_server` 共用；仅在不与 server 合一时才使用独立文件，顶层为 `{"artifact_storage": { ... }}`（勿提交密钥）。
- [`artifact_sync.py`](artifact_sync.py)：子命令 `push` / `pull` / `switch` / `versions` / `print-rclone-config`。
- [`artifact-sync.sh`](artifact-sync.sh)：转发至 `artifact_sync.py`（可选）。

依赖：Python 3 标准库 `json`；云上 `pull`/`push` 需本机安装 [`rclone`](https://rclone.org/install/)。

## 路径约定

- 每版本目录：`{artifact_root_or_bucket}/{env}/{key_prefix}/{table}/{build_id}/`
- **`env` / `key_prefix`**：与表名、`build_id` 一起拼出对象前缀；**不改变 yikv 里的逻辑表名**（`push --table` / `reload` 仍用业务表名）。`key_prefix` 用于在同一制品根下区分产品线或项目目录（例如 `yikv-index`、`search-index`），bucket 共用时也可靠前缀隔离。
- **无**中央指针文件；`pull` 不带 `--build-id` 时会在制品库里 **枚举子目录**，取 **`max(build_id)`** 作为「当前最新」。

## 常用流程

1. **`push`**：上传某一构建（省略 `--build-id` 则用本地时间 **`YYYYMMDDHHmmss`**）。
2. **`pull`**：**只拉一个**版本——默认拉 **最新**；或 **`--build-id`** 拉指定版。可选 **`--max-local-versions`**（默认 2）在 `--dest` 下只保留字典序最大的 N 个版本子目录，便于本机留两版做回滚。
3. **`switch`**（**仅需本地**，**不要** `-c`）：在 **`pull` 的同一 `--dest`** 下，把符号链接 **`active`** 原子指向某一 `build_id/`；省略 `--build-id` 时指向 **本机已有的字典序最大**子目录（通常即最新已下载）。**切换即改链接，不下载。**
4. **让已运行的 `yikv_server` 跟到新目录**（无需重启进程）：在 `config.json` 配置 **`admin_unix_socket`**，发布节点在 `pull`/`switch` 之后对该 socket 发送一行 **`reload <表名>`**（见 [`../../db.md`](../../db.md) §3）。新 RPC 会在重载完成后走新 mmap；进行中的请求会在旧 slot 上跑完后再卸载旧索引。

**多表布局**：`db_path` 为库根；每个 **`{db_path}/{表名}/`** 为指向该表制品目录下 **`active`** 的符号链接（`active` 再指向某一 `build_id/`），这样表名不变、只换 `active` 目标即可发布。

## 示例：`dsp_test4`

```bash
cd /path/to/yikv-server
# 写入最小 artifact-storage.json（或从 config.example.json 拷贝 artifact_storage 段并包一层顶层键）
cat > artifact-storage.json <<'EOF'
{
  "artifact_storage": {
    "provider": "local",
    "env": "dev",
    "key_prefix": "yikv-index",
    "local": { "root": "/data/yikv-artifact-store" }
  }
}
EOF
# 按需编辑 local.root / s3_compatible 等

BUILD_ID=$(python3 tools/artifact_sync/artifact_sync.py -c artifact-storage.json push \
  --table dsp_test4 \
  --source /data/yikv_data/dsp_test4)

# 节点：拉最新 + 设 active（二合一）
python3 tools/artifact_sync/artifact_sync.py -c artifact-storage.json pull \
  --table dsp_test4 \
  --dest /var/lib/yikv/releases/dsp_test4 \
  --switch-active

# 稍后要切到本地已有旧版（需已 pull 过该 build_id）
python3 tools/artifact_sync/artifact_sync.py switch \
  --dest /var/lib/yikv/releases/dsp_test4 \
  --build-id 20260510120000

# 再回到本机目录里最新的那一版（只改链接）
python3 tools/artifact_sync/artifact_sync.py switch \
  --dest /var/lib/yikv/releases/dsp_test4

# 告诉本机 yikv_server：重新打开 dsp_test4（config 里已配 admin_unix_socket）
printf 'reload dsp_test4\n' | socat - UNIX-CONNECT:/var/run/yikv-admin.sock
```

若未安装 `socat`，可用 `nc -U`（视发行版而定）或自写几行 Python `socket(AF_UNIX)` 发送同上文本。

查看制品库有哪些版本：`python3 tools/artifact_sync/artifact_sync.py -c artifact-storage.json versions --table dsp_test4`

## 命令摘要

```bash
# 上传（自动生成时间版本号）
python3 tools/artifact_sync/artifact_sync.py -c artifact-storage.json push \
  --table mytable --source /path/to/db_path/mytable

# 只拉「制品库里最新」的一版（max build_id）
python3 tools/artifact_sync/artifact_sync.py -c artifact-storage.json pull \
  --table mytable --dest /var/lib/yikv/releases/mytable

# 拉指定版
python3 tools/artifact_sync/artifact_sync.py -c artifact-storage.json pull \
  --table mytable --dest /var/lib/yikv/releases/mytable \
  --build-id 20260510101030

# 拉完后把 active 指过去
python3 tools/artifact_sync/artifact_sync.py -c artifact-storage.json pull \
  --table mytable --dest /var/lib/yikv/releases/mytable --switch-active

# 仅切换 active（需 dest 下已有对应子目录）
python3 tools/artifact_sync/artifact_sync.py switch \
  --dest /var/lib/yikv/releases/mytable \
  --build-id 20260510101030

# 不拉对象，只把 active 指到本地最新的子目录
python3 tools/artifact_sync/artifact_sync.py switch --dest /var/lib/yikv/releases/mytable

# 不在 dest 保留多版时
python3 tools/artifact_sync/artifact_sync.py -c artifact-storage.json pull \
  --table mytable --dest /var/lib/yikv/releases/mytable --max-local-versions 1
```

## 安全

密钥优先用 `access_key_id_env` / `secret_access_key_env`；勿把 AK/SK 写入 Git。
