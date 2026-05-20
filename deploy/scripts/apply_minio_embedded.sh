#!/usr/bin/env bash
# Apply deploy/scripts/minio-embedded.yaml after substituting NAMESPACE_PLACEHOLDER.
# Raw YAML must not be applied with: kubectl apply -f minio-embedded.yaml
#
# Kind + static PV (minio-hostpath / minio-pv): this manifest still has storageClassName: local-path.
# Use deploy/k8s/start_minio_kind_cluster.sh for that path; it rewrites the PVC for Kind.
#
# Usage (from anywhere):
#   ./deploy/scripts/apply_minio_embedded.sh
#   MINIO_NAMESPACE=yikv /path/to/apply_minio_embedded.sh

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MANIFEST="${SCRIPT_DIR}/minio-embedded.yaml"
NS="${MINIO_NAMESPACE:-minio}"

if [[ ! -f "${MANIFEST}" ]]; then
  echo "apply_minio_embedded: missing ${MANIFEST}" >&2
  exit 1
fi

kubectl create namespace "${NS}" --dry-run=client -o yaml | kubectl apply -f -

# MinIO namespace names are typically DNS labels; keep sed simple.
sed "s/NAMESPACE_PLACEHOLDER/${NS}/g" "${MANIFEST}" | kubectl apply -f -

echo "apply_minio_embedded: applied MinIO manifests to namespace \"${NS}\"."
