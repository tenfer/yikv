#!/usr/bin/env bash
# One-shot: Kind cluster + MinIO + Kafka + deploy yikv-server (e2e overlay).
# Does NOT run the publish Job (needs your data in MinIO). See README.md.
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
"${SCRIPT_DIR}/bootstrap-kind.sh"
"${SCRIPT_DIR}/deploy-online.sh"
echo ""
echo "quickstart: online yikv deployed. Next:"
echo "  Upload Parquet/CSV to MinIO (prefix in examples/publish-spec.example.json), then:"
echo "  ${SCRIPT_DIR}/run-publish-job.sh --wait --spec-file ${SCRIPT_DIR}/examples/publish-spec.example.json"
