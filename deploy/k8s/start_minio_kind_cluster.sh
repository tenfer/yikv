#!/usr/bin/env bash
# Create a one-node Kind cluster and deploy embedded MinIO (replicas: 1).
# Requires: kind, kubectl, Docker (or rootful Podman with KIND_EXPERIMENTAL_PROVIDER), python3.
#
# Kind storage: uses a static hostPath PV (minio-pv-kind.yaml) + PVC volumeName binding.
# This avoids rancher/local-path-provisioner, which needs in-cluster access to the API at
# 10.96.0.1 — on broken/sandboxed Kind networks that can time out (CrashLoopBackOff).
#
# Migrating from local-path / different PVC spec: Kubernetes will not patch storageClassName,
# volumeName, or shrink storage. Either delete the old PVC (and deployment) or run once with:
#   KIND_MINIO_RECREATE_PVC=1 ./start_minio_kind_cluster.sh
# Or manually:
#   kubectl delete deploy -n minio minio --ignore-not-found
#   kubectl delete pvc -n minio minio-data --ignore-not-found
#
# Usage:
#   ./start_minio_kind_cluster.sh
#   KIND_CLUSTER_NAME=minio MINIO_NAMESPACE=minio ./start_minio_kind_cluster.sh
# To re-apply only MinIO YAML to an existing cluster (non-Kind or same cluster), use:
#   MINIO_NAMESPACE=minio ./deploy/scripts/apply_minio_embedded.sh
#
# Access S3 API from host:
#   kubectl port-forward -n "$MINIO_NAMESPACE" svc/minio 9000:9000
# Web Console (login = root-user / root-password from Secret):
#   kubectl port-forward -n "$MINIO_NAMESPACE" svc/minio 9001:9001
#   then open http://127.0.0.1:9001
# LAN exposure (default on): pod hostPorts 30900 (S3) / 30901 (Console) + Kind extraPortMappings (see kind-minio-single-node.yaml).
# Disable: KIND_EXPOSE_MINIO_NODEPORT=0
# Default credentials: minio / minio12345 (dev only; see minio-embedded.yaml)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
YIKV_SERVER_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
MANIFEST="${YIKV_SERVER_ROOT}/deploy/scripts/minio-embedded.yaml"
KIND_CONFIG="${SCRIPT_DIR}/kind-minio-single-node.yaml"

KIND_CLUSTER_NAME="${KIND_CLUSTER_NAME:-minio}"
MINIO_NAMESPACE="${MINIO_NAMESPACE:-minio}"
KIND_EXPOSE_MINIO_NODEPORT="${KIND_EXPOSE_MINIO_NODEPORT:-1}"

_render_minio_manifest_for_kind() {
  # Use a quoted heredoc so bash does not interpret backticks in the Python source (e.g. in error text).
  MINIO_NS="${MINIO_NAMESPACE}" MINIO_MANIFEST="${MANIFEST}" \
    KIND_EXPOSE_MINIO_NODEPORT="${KIND_EXPOSE_MINIO_NODEPORT}" python3 <<'PY'
import os
import pathlib
import sys

ns = os.environ["MINIO_NS"]
path = pathlib.Path(os.environ["MINIO_MANIFEST"])
text = path.read_text(encoding="utf-8")
text = text.replace("NAMESPACE_PLACEHOLDER", ns)
old = "  storageClassName: local-path\n"
new = "  storageClassName: minio-hostpath\n  volumeName: minio-pv\n"
if old not in text:
    print(
        "start_minio_kind_cluster: expected 'storageClassName: local-path' in minio-embedded.yaml",
        file=sys.stderr,
    )
    sys.exit(1)
text = text.replace(old, new, 1)

# Use hostPort on the Kind node (does not rely on kube-proxy). Off when KIND_EXPOSE_MINIO_NODEPORT=0.
if os.environ.get("KIND_EXPOSE_MINIO_NODEPORT", "1") == "1":
    needle = (
        "          ports:\n"
        "            - containerPort: 9000\n"
        "              name: api\n"
        "            - containerPort: 9001\n"
        "              name: console\n"
    )
    repl = (
        "          ports:\n"
        "            - containerPort: 9000\n"
        "              hostPort: 30900\n"
        "              name: api\n"
        "            - containerPort: 9001\n"
        "              hostPort: 30901\n"
        "              name: console\n"
    )
    if needle not in text:
        print(
            "start_minio_kind_cluster: could not inject hostPorts for MinIO (ports stanza changed?)",
            file=sys.stderr,
        )
        sys.exit(1)
    text = text.replace(needle, repl, 1)

sys.stdout.write(text)
PY
}

if ! command -v kind >/dev/null 2>&1; then
  echo "start_minio_kind_cluster: install kind (https://kind.sigs.k8s.io/)." >&2
  exit 1
fi
if ! command -v kubectl >/dev/null 2>&1; then
  echo "start_minio_kind_cluster: kubectl not found." >&2
  exit 1
fi

if ! command -v python3 >/dev/null 2>&1; then
  echo "start_minio_kind_cluster: python3 required to render MinIO manifest for Kind." >&2
  exit 1
fi

if [[ ! -f "${MANIFEST}" ]]; then
  echo "start_minio_kind_cluster: missing ${MANIFEST}" >&2
  exit 1
fi
if [[ ! -f "${SCRIPT_DIR}/minio-pv-kind.yaml" ]]; then
  echo "start_minio_kind_cluster: missing ${SCRIPT_DIR}/minio-pv-kind.yaml" >&2
  exit 1
fi
if kind get clusters 2>/dev/null | grep -qx "${KIND_CLUSTER_NAME}"; then
  echo "Kind cluster '${KIND_CLUSTER_NAME}' already exists; skipping kind create."
else
  kind create cluster --name "${KIND_CLUSTER_NAME}" --config "${KIND_CONFIG}"
fi

kubectl config use-context "kind-${KIND_CLUSTER_NAME}"

kubectl create namespace "${MINIO_NAMESPACE}" --dry-run=client -o yaml | kubectl apply -f -

if [[ "${KIND_MINIO_RECREATE_PVC:-0}" == "1" ]]; then
  echo "KIND_MINIO_RECREATE_PVC=1: removing MinIO deployment, minio-data PVC, and minio-pv (reclaimed in-cluster; hostPath dir on the Kind node may still hold files)."
  kubectl delete deployment minio -n "${MINIO_NAMESPACE}" --ignore-not-found --wait=true
  kubectl delete pvc minio-data -n "${MINIO_NAMESPACE}" --ignore-not-found --wait=true
  kubectl delete pv minio-pv --ignore-not-found --wait=true
fi

echo "Applying static PersistentVolume for MinIO (hostPath on Kind node; no dynamic provisioner)..."
kubectl apply -f "${SCRIPT_DIR}/minio-pv-kind.yaml"

_render_minio_manifest_for_kind | kubectl apply -f -

# Remove legacy NodePort Service if present (hostPort is used instead).
kubectl delete svc minio-nodeport -n "${MINIO_NAMESPACE}" --ignore-not-found

echo "Waiting for PVC minio-data to become Bound..."
if ! kubectl wait --for=jsonpath='{.status.phase}'=Bound "pvc/minio-data" -n "${MINIO_NAMESPACE}" --timeout=120s; then
  echo "" >&2
  echo "start_minio_kind_cluster: PVC minio-data still Pending. Check:" >&2
  echo "  - PV exists: kubectl get pv minio-pv" >&2
  echo "  - PVC matches PV storageClass (minio-hostpath) and volumeName minio-pv" >&2
  echo "" >&2
  kubectl describe pv minio-pv >&2 || true
  kubectl describe pvc -n "${MINIO_NAMESPACE}" minio-data >&2 || true
  kubectl get events -n "${MINIO_NAMESPACE}" --sort-by=.lastTimestamp | tail -25 >&2 || true
  exit 1
fi

ROLLOUT_TIMEOUT="${ROLLOUT_TIMEOUT:-600s}"
if ! kubectl rollout status "deployment/minio" -n "${MINIO_NAMESPACE}" --timeout="${ROLLOUT_TIMEOUT}"; then
  echo "" >&2
  echo "start_minio_kind_cluster: rollout timed out or failed. Check the following:" >&2
  echo "  - PVC must be Bound:  kubectl get pvc -n ${MINIO_NAMESPACE}" >&2
  echo "  - If Pending:          kubectl describe pvc/pv minio-data minio-pv" >&2
  echo "  - Pod events / logs:   kubectl describe pod -n ${MINIO_NAMESPACE} -l app.kubernetes.io/name=minio" >&2
  echo "                         kubectl logs -n ${MINIO_NAMESPACE} -l app.kubernetes.io/name=minio --tail=100" >&2
  echo "" >&2
  kubectl get pods,pvc -n "${MINIO_NAMESPACE}" -o wide >&2 || true
  kubectl describe pvc -n "${MINIO_NAMESPACE}" minio-data >&2 || true
  kubectl describe pod -n "${MINIO_NAMESPACE}" -l app.kubernetes.io/name=minio >&2 || true
  kubectl logs -n "${MINIO_NAMESPACE}" -l app.kubernetes.io/name=minio --tail=100 >&2 || true
  exit 1
fi

echo ""
echo "MinIO is up (namespace=${MINIO_NAMESPACE}, cluster=${KIND_CLUSTER_NAME})."
echo "  context: kind-${KIND_CLUSTER_NAME}"
echo "  S3 API port-forward:  kubectl port-forward -n ${MINIO_NAMESPACE} svc/minio 9000:9000"
echo "  Web Console UI:      kubectl port-forward -n ${MINIO_NAMESPACE} svc/minio 9001:9001  →  http://127.0.0.1:9001"
echo "  in-cluster API:      http://minio.${MINIO_NAMESPACE}.svc.cluster.local:9000"
if [[ "${KIND_EXPOSE_MINIO_NODEPORT}" == "1" ]]; then
  echo "  remote S3 (hostPort + Kind map, HTTP): http://<docker-host>:30900  (LAN IP; TLS off)"
  echo "  remote Console (HTTP):                  http://<docker-host>:30901  — same login as Secret (dev: minio / minio12345)"
fi
echo "  optional bootstrap JSON: python3 ${YIKV_SERVER_ROOT}/deploy/scripts/bootstrap_minio.py \\"
echo "    --namespace ${MINIO_NAMESPACE} --bucket yikv --minio-s3-host 127.0.0.1"
echo "    (run port-forward first; omit --minio-s3-host if the script runs inside the cluster)"
