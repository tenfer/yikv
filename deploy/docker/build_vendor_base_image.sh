#!/usr/bin/env bash
# Tag `builder-base` only — toolchain + Bazel layer for reuse/cache (no vendor snapshot stage).
# Context: yikv-server repository root (same as deploy/docker/Dockerfile).
#
# Usage:
#   ./deploy/docker/build_vendor_base_image.sh
#   YIKV_VENDOR_BASE_TAG=myregistry/yikv-builder-base:abc123 ./deploy/docker/build_vendor_base_image.sh
#
# Default tag includes first 12 hex chars of sha256(MODULE.bazel.lock).

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
YIKV_SERVER_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
LOCK="${YIKV_SERVER_ROOT}/MODULE.bazel.lock"
[[ -f "${LOCK}" ]]

DIGEST12="$(sha256sum "${LOCK}" | awk '{ print substr($1,1,12) }')"
TAG="${YIKV_VENDOR_BASE_TAG:-yikv-bazel-builder-base:${DIGEST12}}"

export DOCKER_BUILDKIT=1

echo "==> docker build --target builder-base -t ${TAG}" >&2
exec docker build \
  -f "${YIKV_SERVER_ROOT}/deploy/docker/Dockerfile" \
  --target builder-base \
  -t "${TAG}" \
  "${YIKV_SERVER_ROOT}"
