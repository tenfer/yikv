#!/usr/bin/env bash
# Create Kind cluster + MinIO + Kafka + bucket (full infra stack for local e2e).
#
# Usage:
#   ./bootstrap-kind.sh
#   KIND_CLUSTER_NAME=yikv-e2e ./bootstrap-kind.sh
#
# WSL/Docker restart: if the cluster name exists but the API is unreachable, this script deletes
# and recreates the cluster (do not "docker start" old kindest/node containers).
#
# Optional persistence across kind delete/recreate (objects stay on the host):
#   E2E_PERSIST_MINIO_DATA=1 ./bootstrap-kind.sh
#   E2E_MINIO_HOST_DATA_DIR=/path/on/host   # default: $HOME/yikv-e2e-data/minio
#
# Next: ./deploy-online.sh && ./run-publish-job.sh ...

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
KIND_CLUSTER_NAME="${KIND_CLUSTER_NAME:-yikv-e2e}"
# Same variables as bootstrap-infra.sh (must agree for MinIO hostPath ↔ Kind extraMounts).
E2E_PERSIST_MINIO_DATA="${E2E_PERSIST_MINIO_DATA:-0}"
E2E_MINIO_HOST_DATA_DIR="${E2E_MINIO_HOST_DATA_DIR:-${HOME}/yikv-e2e-data/minio}"

_need_cmd() {
  command -v "$1" >/dev/null 2>&1 || {
    echo "bootstrap-kind: need '$1' in PATH" >&2
    exit 1
  }
}

_kind_config_for_create() {
  if [[ "${E2E_PERSIST_MINIO_DATA}" == "1" ]]; then
    mkdir -p "${E2E_MINIO_HOST_DATA_DIR}"
    local abs tmp
    abs="$(cd "${E2E_MINIO_HOST_DATA_DIR}" && pwd)"
    tmp="$(mktemp "${TMPDIR:-/tmp}/yikv-kind-config.XXXXXX.yaml")"
    _YIKV_KIND_HOST_MOUNT_ABS="${abs}" _YIKV_KIND_CFG_OUT="${tmp}" _YIKV_KIND_CFG_IN="${SCRIPT_DIR}/kind-config-persist.yaml.in" python3 <<'PY'
import json
import os
from pathlib import Path

src = Path(os.environ["_YIKV_KIND_CFG_IN"])
dst = Path(os.environ["_YIKV_KIND_CFG_OUT"])
text = src.read_text(encoding="utf-8")
ph = "__E2E_MINIO_HOST_DATA_DIR__"
if ph not in text:
    raise SystemExit("bootstrap-kind: kind-config-persist.yaml.in missing placeholder")
quoted = json.dumps(os.environ["_YIKV_KIND_HOST_MOUNT_ABS"])
dst.write_text(text.replace(ph, quoted), encoding="utf-8")
PY
    echo "${tmp}"
  else
    echo "${SCRIPT_DIR}/kind-config.yaml"
  fi
}

_kind_create_cluster() {
  local cfg
  cfg="$(_kind_config_for_create)"
  echo "==> kind create cluster --name ${KIND_CLUSTER_NAME}"
  if [[ "${E2E_PERSIST_MINIO_DATA}" == "1" ]]; then
    echo "    (E2E_PERSIST_MINIO_DATA=1: MinIO data dir on host: ${E2E_MINIO_HOST_DATA_DIR})"
  fi
  kind create cluster --name "${KIND_CLUSTER_NAME}" --config "${cfg}"
  if [[ "${cfg}" != "${SCRIPT_DIR}/kind-config.yaml" ]]; then
    rm -f "${cfg}"
  fi
}

_ensure_kube_context() {
  kind export kubeconfig --name "${KIND_CLUSTER_NAME}" >/dev/null 2>&1 || true
  kubectl config use-context "kind-${KIND_CLUSTER_NAME}"
}

_cluster_api_ok() {
  kubectl cluster-info --request-timeout=15s >/dev/null 2>&1
}

_kind_control_plane_id() {
  docker ps -qf "name=${KIND_CLUSTER_NAME}-control-plane" 2>/dev/null | head -1
}

_kind_node_has_minio_persist_mount() {
  local cid
  cid="$(_kind_control_plane_id)"
  # No container id (non-Docker backend or docker CLI missing): skip verify.
  [[ -n "${cid}" ]] || return 0
  docker inspect "${cid}" --format '{{range .Mounts}}{{println .Destination}}{{end}}' 2>/dev/null | grep -qx '/mnt/yikv-e2e-minio'
}

_need_cmd kind
_need_cmd kubectl

export E2E_PERSIST_MINIO_DATA
export E2E_MINIO_HOST_DATA_DIR

if kind get clusters 2>/dev/null | grep -qx "${KIND_CLUSTER_NAME}"; then
  _ensure_kube_context
  if ! _cluster_api_ok; then
    echo "bootstrap-kind: cluster '${KIND_CLUSTER_NAME}' exists but API is unreachable (common after WSL/Docker restart)."
    echo "  Deleting and recreating the cluster. Do not use 'docker start' on kindest/node containers."
    kind delete cluster --name "${KIND_CLUSTER_NAME}"
  else
    echo "bootstrap-kind: cluster '${KIND_CLUSTER_NAME}' is healthy; skipping kind create."
    if [[ "${E2E_PERSIST_MINIO_DATA}" == "1" ]] && ! _kind_node_has_minio_persist_mount; then
      echo "bootstrap-kind: E2E_PERSIST_MINIO_DATA=1 but the Kind control-plane container does not mount /mnt/yikv-e2e-minio." >&2
      echo "  This cluster was likely created without host MinIO persistence. Run:" >&2
      echo "    kind delete cluster --name ${KIND_CLUSTER_NAME} && E2E_PERSIST_MINIO_DATA=1 \"$0\"" >&2
      exit 1
    fi
  fi
fi

if ! kind get clusters 2>/dev/null | grep -qx "${KIND_CLUSTER_NAME}"; then
  _kind_create_cluster
  _ensure_kube_context
fi

_ensure_kube_context
if ! _cluster_api_ok; then
  echo "bootstrap-kind: context kind-${KIND_CLUSTER_NAME} is not usable (API unreachable)." >&2
  echo "  Try: kind export kubeconfig --name ${KIND_CLUSTER_NAME}" >&2
  echo "  Then: kind delete cluster --name ${KIND_CLUSTER_NAME} && \"$0\"" >&2
  exit 1
fi

exec "${SCRIPT_DIR}/bootstrap-infra.sh"
