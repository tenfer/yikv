#!/usr/bin/env bash
# Build image + apply e2e Kustomize overlay. From repo root (one command).
# deploy/k8s/deploy.sh auto-runs kind load when kubectl context is kind-* (no REGISTRY).
#
# Usage:
#   ./deploy/e2e/deploy-online.sh

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
YIKV_SERVER_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
export OVERLAY="${OVERLAY:-${YIKV_SERVER_ROOT}/deploy/k8s/overlays/e2e}"

exec "${YIKV_SERVER_ROOT}/deploy/k8s/deploy.sh" "$@"
