#!/usr/bin/env bash
# Generate ~100M product rows (schema: deploy/e2e/fixtures/product/schema.json) as one
# Parquet file using chunked ParquetWriter, then upload to MinIO under e2e-input/product/.
#
# Prerequisites:
#   - ~/.venv/bin/python with pyarrow, numpy
#   - MinIO reachable: default S3_ENDPOINT=http://127.0.0.1:30900 (Kind)；或 port-forward 9000 时覆盖
#   - Dozens of GB free disk for the Parquet
#
# Usage:
#   OUTPUT=/data/big/product_100m.parquet ./deploy/e2e/scripts/generate_product_100m_to_minio.sh
#
# Environment:
#   PRODUCT_ROWS     default 100000000
#   OUTPUT           default /tmp/yikv_product_100m.parquet
#   CHUNK_ROWS       default 1000000
#   S3_ENDPOINT etc. see upload_product_fixture_to_minio.sh
#   OBJECT_KEY         default e2e-input/product/product_100m.parquet
#   SKIP_GENERATE=1    only upload existing OUTPUT
#   SKIP_UPLOAD=1      only generate

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

PRODUCT_ROWS="${PRODUCT_ROWS:-100000000}"
OUTPUT="${OUTPUT:-/tmp/yikv_product_100m.parquet}"
CHUNK_ROWS="${CHUNK_ROWS:-1000000}"
OBJECT_KEY="${OBJECT_KEY:-e2e-input/product/product_100m.parquet}"

PYTHON="${PYTHON:-python3}"
if [[ -x "${HOME}/.venv/bin/python" ]]; then
  PYTHON="${HOME}/.venv/bin/python"
fi
export PYTHONUNBUFFERED="${PYTHONUNBUFFERED:-1}"

if [[ "${SKIP_GENERATE:-0}" != "1" ]]; then
  echo "==> generating ${PRODUCT_ROWS} rows -> ${OUTPUT} (chunk=${CHUNK_ROWS})" >&2
  "${PYTHON}" "${SCRIPT_DIR}/generate_product_parquet.py" \
    --rows "${PRODUCT_ROWS}" \
    -o "${OUTPUT}" \
    --chunk-rows "${CHUNK_ROWS}"
else
  [[ -f "${OUTPUT}" ]] || { echo "SKIP_GENERATE=1 but missing ${OUTPUT}" >&2; exit 1; }
fi

if [[ "${SKIP_UPLOAD:-0}" != "1" ]]; then
  echo "==> upload to MinIO" >&2
  OBJECT_KEY="${OBJECT_KEY}" "${SCRIPT_DIR}/upload_product_fixture_to_minio.sh" "${OUTPUT}"
else
  echo "==> SKIP_UPLOAD=1, file at ${OUTPUT}" >&2
fi

echo "done" >&2
