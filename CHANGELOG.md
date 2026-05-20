# Changelog

本文件记录面向操作者与下游的显著变更；与 **git tag semver**、`MODULE.bazel` 的 `module(version)` 发版时请同步更新本节。

## [Unreleased]

### BREAKING

- **构建 / 布局**：`yikv` 引擎源码合入 **`libs/yikv/`**；已不再使用 **`bazel_dep` + `local_path_override(../yikv)`** 的二模块布局。单机开发请使用本仓库克隆 + 可选 **`scripts/sync_yikv_lib.sh`** 同步上游 `yikv`。
- **Docker**：镜像构建上下文由「含 `yikv/`、`yikv-server/` 的父目录」改为 **`yikv-server` 仓库根**；沿用父目录将导致 `COPY`/路径不匹配。参见根目录 **[`.dockerignore`](.dockerignore)** 与 [`deploy/docker/Dockerfile`](deploy/docker/Dockerfile)。

### Added

- **`//libs/yikv:yikv_core`**：对外聚合 Bazel target（见 **`docs/YIKV_CORE_API.md`**）。
- **`scripts/sync_yikv_lib.sh`**：`YIKV_SOURCE` 可选，默认同步同级 `../yikv`。

### Changed

- **单 `brpc` 外链**：沿用根 **`MODULE.bazel`** 内 **`local_tarball(com_github_apache_brpc)`**；移除子模块级重复声明（随 `MODULE.bazel` 删除）。
- **引擎单测**：`//libs/yikv/tests:all_tests` 与各 benchmark 目标的包路径已迁至 `libs/yikv` 树下。
