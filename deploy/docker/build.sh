#!/usr/bin/env bash
# Build images with context = yikv-server repository root (see deploy/docker/Dockerfile).
# Do not run plain `docker build` from deploy/docker/ alone — context would omit MODULE.bazel / libs/yikv.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "${ROOT}"

if command -v docker >/dev/null 2>&1; then
  RUNTIME=docker
elif command -v podman >/dev/null 2>&1; then
  RUNTIME=podman
else
  echo "yikv-server/deploy/docker/build.sh: neither 'docker' nor 'podman' found in PATH." >&2
  echo "Install one of them (e.g. sudo apt install podman docker.io) or add it to PATH." >&2
  exit 1
fi

exec "${RUNTIME}" build -f deploy/docker/Dockerfile "$@" .
