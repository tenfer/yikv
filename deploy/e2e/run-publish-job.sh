#!/usr/bin/env bash
# Build pipeline-worker from source, load into Kind, create/update ConfigMap, submit a publish Job.
#
# Usage (repo root):
#   ./deploy/e2e/run-publish-job.sh --spec-file path/to/publish-spec.json
#   ./deploy/e2e/run-publish-job.sh   # uses example spec (needs MinIO data at s3://...)
#
# Reuse a fixed image (skip Dockerfile build; default still builds every run so binaries match git):
#   SKIP_BUILD=1 IMAGE_NAME=myregistry/pipeline-worker TAG=v1.2.3 ./deploy/e2e/run-publish-job.sh --spec-file ... --wait
#   - Non-Kind: cluster pulls IMAGE_NAME:TAG (configure imagePullSecrets if private).
#   - Kind: image must exist locally (prior build or docker pull) so kind load can import it.
#
# Requires: docker or podman (for build and/or kind load), kind when context is kind-*; kubectl; pip kubernetes client.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
YIKV_SERVER_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
DOCKER_CONTEXT="${YIKV_SERVER_ROOT}"
CONFIG_JSON="${CONFIG_JSON:-${YIKV_SERVER_ROOT}/deploy/k8s/overlays/e2e/server-config.json}"
SPEC_FILE=""
IMAGE_NAME="${IMAGE_NAME:-pipeline-worker}"
TAG="${TAG:-latest}"
NAMESPACE="${NAMESPACE:-yikv}"
CM_NAME="${PUBLISH_CONFIGMAP:-yikv-e2e-publish-config}"
WAIT="${WAIT:-0}"
PASSTHROUGH=()

while [[ $# -gt 0 ]]; do
  case "$1" in
    --spec-file)
      SPEC_FILE="$2"
      shift 2
      ;;
    --wait)
      WAIT=1
      shift
      ;;
    *)
      PASSTHROUGH+=("$1")
      shift
      ;;
  esac
done

if [[ -z "${SPEC_FILE}" ]]; then
  SPEC_FILE="${SCRIPT_DIR}/examples/publish-spec.example.json"
  echo "run-publish-job: using example spec ${SPEC_FILE} (override with --spec-file)" >&2
fi
[[ -f "${SPEC_FILE}" ]] || {
  echo "run-publish-job: missing --spec-file ${SPEC_FILE}" >&2
  exit 1
}
[[ -f "${CONFIG_JSON}" ]] || {
  echo "run-publish-job: missing CONFIG_JSON ${CONFIG_JSON}" >&2
  exit 1
}

if command -v docker >/dev/null 2>&1; then
  DOCKER=docker
elif command -v podman >/dev/null 2>&1; then
  DOCKER=podman
else
  echo "run-publish-job: need docker or podman" >&2
  exit 1
fi

FULL_IMAGE="${IMAGE_NAME}:${TAG}"

SKIP_BUILD="${SKIP_BUILD:-0}"
ctx="$(kubectl config current-context 2>/dev/null || true)"

export DOCKER_BUILDKIT=1

# Optional: limit Bazel parallel load inside the image build (WSL / low-RAM).
# Note: `docker build` (buildx) does not support --cpus/--memory; cap resources via
# BAZEL_JOBS / BAZEL_LOCAL_RAM_RESOURCES, Docker Desktop limits, or .wslconfig.
DOCKER_BUILD_ARGS=()
[[ -n "${BAZEL_JOBS:-}" ]] && DOCKER_BUILD_ARGS+=(--build-arg "BAZEL_JOBS=${BAZEL_JOBS}")
[[ -n "${BAZEL_LOCAL_RAM_RESOURCES:-}" ]] && DOCKER_BUILD_ARGS+=(--build-arg "BAZEL_LOCAL_RAM_RESOURCES=${BAZEL_LOCAL_RAM_RESOURCES}")
[[ -n "${BAZEL_VENDOR_CONFIG:-}" ]] && DOCKER_BUILD_ARGS+=(--build-arg "BAZEL_VENDOR_CONFIG=${BAZEL_VENDOR_CONFIG}")

if [[ "${SKIP_BUILD}" == 1 ]]; then
  echo "run-publish-job: SKIP_BUILD=1 — skip ${DOCKER} build (image ${FULL_IMAGE})" >&2
  if [[ "${ctx}" == kind-* ]]; then
    if ! "${DOCKER}" image inspect "${FULL_IMAGE}" >/dev/null 2>&1; then
      echo "run-publish-job: Kind needs a local image to load; not found: ${FULL_IMAGE}" >&2
      echo "  Build once without SKIP_BUILD, or: ${DOCKER} pull <registry>/${IMAGE_NAME}:${TAG}" >&2
      exit 1
    fi
  fi
else
  echo "==> ${DOCKER} build --target pipeline-worker ${FULL_IMAGE}"
  "${DOCKER}" build "${DOCKER_BUILD_ARGS[@]}" \
    -f "${YIKV_SERVER_ROOT}/deploy/docker/Dockerfile" \
    --target pipeline-worker \
    -t "${FULL_IMAGE}" \
    "${DOCKER_CONTEXT}"
fi

if [[ "${ctx}" == kind-* ]]; then
  # kind uses docker save → multi-GB tar in TMPDIR. On WSL, /tmp is often tmpfs (~4GiB)
  # while / has hundreds of GB; default TMPDIR would fill tmpfs ("no space left on device").
  if [[ -z "${TMPDIR:-}" ]]; then
    export TMPDIR="${XDG_CACHE_HOME:-$HOME/.cache}/yikv-e2e/kind-tmp"
    mkdir -p "${TMPDIR}"
    echo "run-publish-job: TMPDIR unset → using ${TMPDIR} for kind load (avoid small /tmp tmpfs)" >&2
  fi
  echo "==> kind load docker-image ${FULL_IMAGE}"
  kind load docker-image "${FULL_IMAGE}"
fi

echo "==> ConfigMap ${CM_NAME} from ${CONFIG_JSON}"
kubectl create configmap "${CM_NAME}" -n "${NAMESPACE}" \
  --from-file=config.json="${CONFIG_JSON}" \
  --dry-run=client -o yaml | kubectl apply -f -

SUBMIT_ARGS=(
  python3 "${YIKV_SERVER_ROOT}/deploy/scripts/submit_publish_job.py"
  --namespace "${NAMESPACE}"
  --image "${FULL_IMAGE}"
  --spec-file "${SPEC_FILE}"
  --configmap "${CM_NAME}"
  --work-on-emptydir
  --set-env "AWS_ACCESS_KEY_ID=minio"
  --set-env "AWS_SECRET_ACCESS_KEY=minio12345"
  --set-env "AWS_ENDPOINT_URL_S3=http://minio.yikv.svc.cluster.local:9000"
  --set-env "AWS_REGION=us-east-1"
)
if [[ "${WAIT}" == 1 ]]; then
  SUBMIT_ARGS+=(--wait)
fi

echo "==> submit Job"
"${SUBMIT_ARGS[@]}" "${PASSTHROUGH[@]}"
