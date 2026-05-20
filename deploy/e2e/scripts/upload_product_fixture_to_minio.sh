#!/usr/bin/env bash
# Upload a Parquet file to MinIO under the prefix expected by
# deploy/e2e/examples/publish-spec.product.json (s3://yikv-artifacts/e2e-input/product/).
#
# Prerequisite: MinIO S3 API reachable from this host (e.g.
#   Kind + deploy/e2e/kind-config.yaml: host port 30900 -> http://127.0.0.1:30900 (default S3_ENDPOINT)
#   or kubectl port-forward -n yikv svc/minio 9000:9000 then S3_ENDPOINT=http://127.0.0.1:9000
#   or run this script inside a pod (in-cluster endpoint).
#
# Usage:
#   ./deploy/e2e/scripts/upload_product_fixture_to_minio.sh /path/to/product.parquet
#
# Environment:
#   S3_ENDPOINT            default http://127.0.0.1:30900
#   AWS_ACCESS_KEY_ID      default minio (e2e)
#   AWS_SECRET_ACCESS_KEY  default minio12345
#   BUCKET                 default yikv-artifacts
#   OBJECT_KEY             optional full key under bucket; default e2e-input/product/<basename>

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../../.." && pwd)"

if [[ $# -lt 1 ]] || [[ ! -f "$1" ]]; then
  echo "usage: $0 /path/to/file.parquet" >&2
  echo "  Validate columns first: python3 ${REPO_ROOT}/deploy/e2e/scripts/verify_product_parquet.py \"\$1\"" >&2
  exit 1
fi

FILE="$1"
BUCKET="${BUCKET:-yikv-artifacts}"
S3_ENDPOINT="${S3_ENDPOINT:-http://127.0.0.1:30900}"
export AWS_ACCESS_KEY_ID="${AWS_ACCESS_KEY_ID:-minio}"
export AWS_SECRET_ACCESS_KEY="${AWS_SECRET_ACCESS_KEY:-minio12345}"
export AWS_REGION="${AWS_REGION:-us-east-1}"

PYTHON="${PYTHON:-python3}"
if [[ -x "${HOME}/.venv/bin/python" ]]; then
  PYTHON="${HOME}/.venv/bin/python"
fi

if [[ -n "${OBJECT_KEY:-}" ]]; then
  KEY="${OBJECT_KEY}"
else
  KEY="e2e-input/product/$(basename "${FILE}")"
fi

_aws() {
  aws --endpoint-url "${S3_ENDPOINT}" "$@"
}

if command -v aws >/dev/null 2>&1; then
  if ! _aws s3 ls "s3://${BUCKET}/" >/dev/null 2>&1; then
    echo "==> creating bucket ${BUCKET} (aws s3 mb)" >&2
    _aws s3 mb "s3://${BUCKET}/" || true
  fi
  echo "==> aws s3 cp -> s3://${BUCKET}/${KEY}" >&2
  _aws s3 cp "${FILE}" "s3://${BUCKET}/${KEY}"
elif "${PYTHON}" -c "import minio" 2>/dev/null; then
  echo "==> minio Python SDK -> s3://${BUCKET}/${KEY}" >&2
  "${PYTHON}" "${SCRIPT_DIR}/upload_parquet_minio.py" "${FILE}" \
    --endpoint "${S3_ENDPOINT}" \
    --bucket "${BUCKET}" \
    --key "${KEY}" \
    --access-key "${AWS_ACCESS_KEY_ID}" \
    --secret-key "${AWS_SECRET_ACCESS_KEY}"
elif command -v mc >/dev/null 2>&1; then
  ALIAS="${MC_ALIAS:-yikv-e2e-upload}"
  mc alias set "${ALIAS}" "${S3_ENDPOINT}" "${AWS_ACCESS_KEY_ID}" "${AWS_SECRET_ACCESS_KEY}" >/dev/null
  echo "==> mc cp -> ${ALIAS}/${BUCKET}/${KEY}" >&2
  mc mb -p "${ALIAS}/${BUCKET}" >/dev/null 2>&1 || true
  mc cp "${FILE}" "${ALIAS}/${BUCKET}/${KEY}"
else
  echo "error: need AWS CLI (\`aws\`), Python package \`minio\`, or MinIO Client (\`mc\`) in PATH" >&2
  exit 1
fi

echo "ok: object s3://${BUCKET}/${KEY} (endpoint ${S3_ENDPOINT})" >&2
