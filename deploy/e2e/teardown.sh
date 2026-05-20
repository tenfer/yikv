#!/usr/bin/env bash
# Tear down Kind cluster created by bootstrap-kind.sh (default name yikv-e2e).

set -euo pipefail
KIND_CLUSTER_NAME="${KIND_CLUSTER_NAME:-yikv-e2e}"
kind delete cluster --name "${KIND_CLUSTER_NAME}"
echo "ok: kind cluster ${KIND_CLUSTER_NAME} deleted"
