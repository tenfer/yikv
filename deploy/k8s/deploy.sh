#!/usr/bin/env bash
# One-shot: build runtime image, optionally push, apply Kustomize overlay.
#
# Usage:
#   ./deploy.sh                      # build + kubectl apply (local image name yikv-server:latest)
#   OVERLAY=/path/to/deploy/k8s/overlays/e2e ./deploy.sh   # local e2e (MinIO+Kafka config); see deploy/e2e/README.md
#   REGISTRY=myreg.io ./deploy.sh    # also docker push; image = myreg.io/yikv-server:${TAG:-latest}
#   PUSH=1 ./deploy.sh               # push even if REGISTRY is empty (uses IMAGE_NAME:TAG)
#   KIND_LOAD=1 ./deploy.sh          # kind load docker-image (after build)
#   KIND_LOAD=0 ./deploy.sh         # never kind-load (use registry image)
#   If KIND_LOAD is unset: auto-load when context is kind-* and REGISTRY is empty
#   DOCKER=podman ./deploy.sh       # use Podman instead of Docker
#   UBUNTU_IMAGE=docker.m.daocloud.io/library/ubuntu:22.04 ./deploy.sh   # if docker.io times out
#   BAZEL_DOWNLOAD_PREFIX=https://ghproxy.net/https:// ./deploy.sh
#   USE_BAZEL_GITHUB_MIRROR=1 ./deploy.sh   # optional: ghproxy URL rewrite (often 502; try direct + proxy first)
#   HTTP_PROXY=http://host:port HTTPS_PROXY=http://host:port ./deploy.sh   # passed to bazel build stage
#
# Build context = yikv-server repository root (contains libs/yikv/). Run kubectl against a cluster with default StorageClass.
#
# Requires a container CLI: install Docker or Podman, e.g. on OpenCloudOS / RHEL-like:
#   sudo dnf install -y podman
#   # or: sudo dnf install -y docker-ce docker-ce-cli containerd.io && sudo systemctl enable --now docker

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
YIKV_SERVER_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
DOCKER_CONTEXT="${YIKV_SERVER_ROOT}"

IMAGE_NAME="${IMAGE_NAME:-yikv-server}"
TAG="${TAG:-latest}"
REGISTRY="${REGISTRY:-}"
DOCKERFILE="${DOCKERFILE:-${YIKV_SERVER_ROOT}/deploy/docker/Dockerfile}"
OVERLAY="${OVERLAY:-${YIKV_SERVER_ROOT}/deploy/k8s/overlays/prod}"

resolve_container_cli() {
  if [[ -n "${DOCKER:-}" ]]; then
    command -v "${DOCKER}" >/dev/null 2>&1 || {
      echo "error: DOCKER=${DOCKER} not found in PATH" >&2
      exit 1
    }
    return
  fi
  if command -v docker >/dev/null 2>&1; then
    DOCKER=docker
  elif command -v podman >/dev/null 2>&1; then
    DOCKER=podman
  else
    cat >&2 <<'EOF'
error: neither 'docker' nor 'podman' found in PATH.

Install one, for example on OpenCloudOS / Fedora-like:
  sudo dnf install -y podman

Or Docker Engine (after adding Docker's repo if needed):
  sudo dnf install -y docker-ce docker-ce-cli containerd.io
  sudo systemctl enable --now docker

Then re-run this script, or set DOCKER=/path/to/cli
EOF
    exit 1
  fi
}

resolve_container_cli

if [[ -n "${REGISTRY}" ]]; then
  FULL_IMAGE="${REGISTRY%/}/${IMAGE_NAME}:${TAG}"
else
  FULL_IMAGE="${IMAGE_NAME}:${TAG}"
fi

echo "==> ${DOCKER} build ${FULL_IMAGE} (context: ${DOCKER_CONTEXT})"
BUILD_ARGS=()
if [[ -n "${UBUNTU_IMAGE:-}" ]]; then
  BUILD_ARGS+=(--build-arg "UBUNTU_IMAGE=${UBUNTU_IMAGE}")
  echo "    (UBUNTU_IMAGE=${UBUNTU_IMAGE})"
fi
if [[ -n "${BAZEL_DOWNLOAD_PREFIX:-}" ]]; then
  BUILD_ARGS+=(--build-arg "BAZEL_DOWNLOAD_PREFIX=${BAZEL_DOWNLOAD_PREFIX}")
  echo "    (BAZEL_DOWNLOAD_PREFIX=${BAZEL_DOWNLOAD_PREFIX})"
fi
if [[ -n "${BAZEL_URL:-}" ]]; then
  BUILD_ARGS+=(--build-arg "BAZEL_URL=${BAZEL_URL}")
  echo "    (BAZEL_URL=${BAZEL_URL})"
fi
if [[ -n "${USE_BAZEL_GITHUB_MIRROR:-}" ]]; then
  BUILD_ARGS+=(--build-arg "USE_BAZEL_GITHUB_MIRROR=${USE_BAZEL_GITHUB_MIRROR}")
  echo "    (USE_BAZEL_GITHUB_MIRROR=${USE_BAZEL_GITHUB_MIRROR})"
fi
for _px in HTTP_PROXY HTTPS_PROXY NO_PROXY http_proxy https_proxy no_proxy; do
  _val="${!_px-}"
  if [[ -n "${_val}" ]]; then
    BUILD_ARGS+=(--build-arg "${_px}=${_val}")
    echo "    (passing ${_px} to build)"
  fi
done
"${DOCKER}" build -f "${DOCKERFILE}" "${BUILD_ARGS[@]}" -t "${FULL_IMAGE}" "${DOCKER_CONTEXT}"

if [[ "${PUSH:-0}" == "1" ]] || [[ -n "${REGISTRY}" ]]; then
  echo "==> ${DOCKER} push ${FULL_IMAGE}"
  "${DOCKER}" push "${FULL_IMAGE}"
fi

# Kind local images: resolve cluster name from context kind-foo -> foo (default "kind" misses yikv-e2e).
_kubectl_ctx="$(kubectl config current-context 2>/dev/null || true)"
_should_kind_load=0
if [[ "${KIND_LOAD:-}" == "0" ]]; then
  _should_kind_load=0
elif [[ "${KIND_LOAD:-}" == "1" ]]; then
  _should_kind_load=1
elif [[ "${_kubectl_ctx}" == kind-* ]] && [[ -z "${REGISTRY}" ]] && [[ "${PUSH:-0}" != "1" ]]; then
  _should_kind_load=1
  echo "==> auto kind load (context ${_kubectl_ctx}, no REGISTRY); set KIND_LOAD=0 to skip"
fi

if [[ "${_should_kind_load}" == "1" ]]; then
  command -v kind >/dev/null 2>&1 || {
    echo "error: kind load needed but 'kind' not found in PATH" >&2
    exit 1
  }
  if [[ "${_kubectl_ctx}" == kind-* ]]; then
    _kind_name="${_kubectl_ctx#kind-}"
    echo "==> kind load docker-image ${FULL_IMAGE} --name ${_kind_name}"
    kind load docker-image "${FULL_IMAGE}" --name "${_kind_name}"
  else
    echo "==> kind load docker-image ${FULL_IMAGE}"
    kind load docker-image "${FULL_IMAGE}"
  fi
fi

TMP_KUSTOMIZE="$(mktemp -d)"
cleanup() { rm -rf "${TMP_KUSTOMIZE}"; }
trap cleanup EXIT

# Preserve k8s/base and k8s/overlays/... layout so ../../base in overlay still resolves.
K8S_WORKTREE="${TMP_KUSTOMIZE}/k8s"
mkdir -p "${K8S_WORKTREE}/overlays"
cp -a "${YIKV_SERVER_ROOT}/deploy/k8s/base" "${K8S_WORKTREE}/base"
cp -a "${OVERLAY}/." "${K8S_WORKTREE}/overlays/prod"

export KUSTOMIZE_PATCH_DIR="${K8S_WORKTREE}/overlays/prod"
export TMP_KUSTOMIZE
export IMG_REPO="${FULL_IMAGE%:*}"
export IMG_TAG="${FULL_IMAGE##*:}"

python3 <<'PY'
import os
import pathlib
import re

tmp = os.environ["KUSTOMIZE_PATCH_DIR"]
repo = os.environ["IMG_REPO"]
tag = os.environ["IMG_TAG"]
kf = pathlib.Path(tmp) / "kustomization.yaml"
text = kf.read_text()
text = re.sub(r"(?m)^(\s*newName:)\s*\S+.*$", rf"\1 {repo}", text, count=1)
text = re.sub(r"(?m)^(\s*newTag:)\s*\S+.*$", rf"\1 {tag}", text, count=1)
kf.write_text(text)
PY

echo "==> kubectl apply -k ${K8S_WORKTREE}/overlays/prod"
kubectl apply -k "${K8S_WORKTREE}/overlays/prod"

ROLLOUT_TIMEOUT="${ROLLOUT_TIMEOUT:-600s}"
if ! kubectl rollout status deployment/yikv-server -n yikv --timeout="${ROLLOUT_TIMEOUT}"; then
  echo "" >&2
  echo "deploy.sh: rollout timed out or failed." >&2
  echo "  Typical causes:" >&2
  echo "    - PVC yikv-data Pending → no/default StorageClass; set storageClassName in overlay or cluster admin" >&2
  echo "    - ImagePullBackOff        → push image (REGISTRY=...) or KIND_LOAD=1 + same node as build" >&2
  echo "    - CrashLoop               → config/kafka/permissions; see logs below" >&2
  echo "    - Unschedulable           → insufficient CPU/memory (Deployment requests 500m CPU, 2Gi RAM)" >&2
  echo "    - startupProbe            → RPC :9000 not listening in time (server slow to start)" >&2
  echo "" >&2
  kubectl get pods,pvc -n yikv -o wide >&2 || true
  kubectl get events -n yikv --sort-by=.lastTimestamp | tail -35 >&2 || true
  kubectl describe pod -n yikv -l app.kubernetes.io/name=yikv-server >&2 || true
  kubectl logs -n yikv -l app.kubernetes.io/name=yikv-server --tail=120 >&2 || true
  exit 1
fi

echo "==> ok: rpc at yikv-server.yikv.svc.cluster.local:9000"
