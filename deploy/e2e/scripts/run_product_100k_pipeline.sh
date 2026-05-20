#!/usr/bin/env bash
# Generate 100k product Parquet, upload to MinIO, publishIndex (Job), deployIndex (Job on PVC).
#
# Prerequisites:
#   - kubectl context points at cluster with yikv + minio
#   - MinIO reachable for upload: default S3_ENDPOINT=http://127.0.0.1:30900 (Kind hostPort); override if port-forward 9000
#     (run: kubectl port-forward -n yikv svc/minio 9000:9000)
#   - docker or podman for pipeline-worker image
#   - python3 + kubernetes + pyarrow (repo requirements)
#
# Usage (repo root):
#   ./deploy/e2e/scripts/run_product_100k_pipeline.sh
#
# Note: deployIndex uses /data/admin.sock on the shared PVC. This works when the PV supports
# unix sockets visible to multiple pods on the same node (e.g. many kind/local setups). If reload
# fails, see deploy/e2e/MINIO_PUBLISH_DEPLOY.md and consider pipeline_agent alternatives.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
YIKV_SERVER_ROOT="$(cd "${SCRIPT_DIR}/../../.." && pwd)"
cd "${YIKV_SERVER_ROOT}"
PYTHON="${PYTHON:-python3}"
if [[ -x "${HOME}/.venv/bin/python" ]]; then PYTHON="${HOME}/.venv/bin/python"; fi
export PYTHON

ROWS="${ROWS:-100000}"
S3_ENDPOINT="${S3_ENDPOINT:-http://127.0.0.1:30900}"
NAMESPACE="${NAMESPACE:-yikv}"
SKIP_UPLOAD="${SKIP_UPLOAD:-0}"
SKIP_PUBLISH="${SKIP_PUBLISH:-0}"
SKIP_DEPLOY="${SKIP_DEPLOY:-0}"
BUILD_ID="${BUILD_ID:-}"

CACHE_DIR="${XDG_CACHE_HOME:-$HOME/.cache}/yikv-e2e"
mkdir -p "${CACHE_DIR}"
PARQUET="${CACHE_DIR}/product_${ROWS}.parquet"
SPEC_PUBLISH="${YIKV_SERVER_ROOT}/deploy/e2e/examples/publish-spec.product.json"
CM_NAME="${PUBLISH_CONFIGMAP:-yikv-e2e-publish-config}"
CONFIG_JSON="${CONFIG_JSON:-${YIKV_SERVER_ROOT}/deploy/k8s/overlays/e2e/server-config.json}"
IMAGE_NAME="${IMAGE_NAME:-pipeline-worker}"
TAG="${TAG:-latest}"
FULL_IMAGE="${IMAGE_NAME}:${TAG}"

DEPLOY_SPEC=""
cleanup() { [[ -n "${DEPLOY_SPEC}" ]] && rm -f "${DEPLOY_SPEC}"; }
trap cleanup EXIT

echo "==> 1) generate ${ROWS} rows -> ${PARQUET}"
"${PYTHON}" "${SCRIPT_DIR}/generate_product_parquet.py" --rows "${ROWS}" --self-check -o "${PARQUET}"

if [[ "${SKIP_UPLOAD}" != "1" ]]; then
  echo "==> 2) upload to MinIO (endpoint ${S3_ENDPOINT})"
  export S3_ENDPOINT
  "${SCRIPT_DIR}/upload_product_fixture_to_minio.sh" "${PARQUET}"
else
  echo "==> 2) SKIP_UPLOAD=1 — using existing objects under s3://yikv-artifacts/e2e-input/product/"
fi

if [[ "${SKIP_PUBLISH}" != "1" ]]; then
  echo "==> 3) publishIndex (pipeline-worker Job)"
  export CONFIG_JSON
  if command -v docker >/dev/null 2>&1; then DOCKER=docker
  elif command -v podman >/dev/null 2>&1; then DOCKER=podman
  else echo "error: need docker or podman"; exit 1
  fi
  export DOCKER_BUILDKIT=1
  DOCKER_BUILD_ARGS=()
  [[ -n "${BAZEL_JOBS:-}" ]] && DOCKER_BUILD_ARGS+=(--build-arg "BAZEL_JOBS=${BAZEL_JOBS}")
  [[ -n "${BAZEL_LOCAL_RAM_RESOURCES:-}" ]] && DOCKER_BUILD_ARGS+=(--build-arg "BAZEL_LOCAL_RAM_RESOURCES=${BAZEL_LOCAL_RAM_RESOURCES}")
  "${DOCKER}" build "${DOCKER_BUILD_ARGS[@]}" \
    -f "${YIKV_SERVER_ROOT}/deploy/docker/Dockerfile" \
    --target pipeline-worker \
    -t "${FULL_IMAGE}" \
    "${YIKV_SERVER_ROOT}"
  ctx="$(kubectl config current-context 2>/dev/null || true)"
  if [[ "${ctx}" == kind-* ]]; then
    if [[ -z "${TMPDIR:-}" ]]; then
      export TMPDIR="${XDG_CACHE_HOME:-$HOME/.cache}/yikv-e2e/kind-tmp"
      mkdir -p "${TMPDIR}"
    fi
    _kn="${ctx#kind-}"
    kind load docker-image "${FULL_IMAGE}" --name "${_kn}"
  fi
  kubectl create configmap "${CM_NAME}" -n "${NAMESPACE}" \
    --from-file=config.json="${CONFIG_JSON}" \
    --dry-run=client -o yaml | kubectl apply -f -
  PUB_JOB="$(python3 "${YIKV_SERVER_ROOT}/deploy/scripts/submit_publish_job.py" \
    --namespace "${NAMESPACE}" \
    --image "${FULL_IMAGE}" \
    --spec-file "${SPEC_PUBLISH}" \
    --configmap "${CM_NAME}" \
    --work-on-emptydir \
    --set-env "AWS_ACCESS_KEY_ID=minio" \
    --set-env "AWS_SECRET_ACCESS_KEY=minio12345" \
    --set-env "AWS_ENDPOINT_URL_S3=http://minio.${NAMESPACE}.svc.cluster.local:9000" \
    --set-env "AWS_REGION=us-east-1" \
    --wait)"
  echo "publish Job: ${PUB_JOB}"
  LOG_TAIL="$(kubectl logs -n "${NAMESPACE}" "job/${PUB_JOB}" 2>/dev/null | tail -1 || true)"
  export BUILD_ID
  BUILD_ID="$(python3 -c "import json,sys; print(json.loads(sys.argv[1])['build_id'])" "${LOG_TAIL}")"
  echo "build_id: ${BUILD_ID}"
else
  echo "==> 3) SKIP_PUBLISH=1"
  if [[ -z "${BUILD_ID}" ]]; then
    echo "error: set BUILD_ID when SKIP_PUBLISH=1" >&2
    exit 1
  fi
fi

if [[ "${SKIP_DEPLOY}" != "1" ]]; then
  echo "==> 4) deployIndex (Job on PVC ${NAMESPACE}/yikv-data)"
  [[ -n "${BUILD_ID}" ]] || { echo "error: empty BUILD_ID"; exit 1; }
  DEPLOY_SPEC="$(mktemp)"
  export BUILD_ID
  python3 -c 'import json,os; print(json.dumps({"table":"product","build_id": os.environ["BUILD_ID"]}, ensure_ascii=False))' > "${DEPLOY_SPEC}"
  DEP_JOB="$(python3 "${YIKV_SERVER_ROOT}/deploy/scripts/submit_deploy_job.py" \
    --namespace "${NAMESPACE}" \
    --image "${FULL_IMAGE}" \
    --spec-file "${DEPLOY_SPEC}" \
    --configmap "${CM_NAME}" \
    --pvc-claim yikv-data \
    --set-env "AWS_ACCESS_KEY_ID=minio" \
    --set-env "AWS_SECRET_ACCESS_KEY=minio12345" \
    --set-env "AWS_ENDPOINT_URL_S3=http://minio.${NAMESPACE}.svc.cluster.local:9000" \
    --set-env "AWS_REGION=us-east-1" \
    --wait)"
  echo "deploy Job: ${DEP_JOB}"
  kubectl logs -n "${NAMESPACE}" "job/${DEP_JOB}" | tail -8 || true
else
  echo "==> 4) SKIP_DEPLOY=1"
fi

echo "==> done. Check: kubectl exec -n ${NAMESPACE} deploy/yikv-server -- ls -la /data/db/product"
