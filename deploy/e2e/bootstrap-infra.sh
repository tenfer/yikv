#!/usr/bin/env bash
# e2e: MinIO + Kafka + artifact bucket on an existing Kind cluster (namespace yikv).
#
# Prerequisites: kind cluster exists, kubectl context points at it.
# Usage:
#   ./bootstrap-infra.sh
#   MINIO_NAMESPACE=yikv ./bootstrap-infra.sh   # default
#
# Optional: pip install -r yikv-server/requirements.txt (for bootstrap_minio bucket step)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# deploy/e2e -> yikv-server repo root
YIKV_SERVER_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
MINIO_NAMESPACE="${MINIO_NAMESPACE:-yikv}"
LOCAL_PATH_URL="${LOCAL_PATH_URL:-https://raw.githubusercontent.com/rancher/local-path-provisioner/v0.0.30/deploy/local-path-storage.yaml}"
# 1 = publish MinIO on host 30900 (S3) / 30901 (Console) via pod hostPort + Kind extraPortMappings (see kind-config.yaml).
# 同一局域网其它机器可访问 http://<运行 Kind 的主机 IP>:30900。防火墙需放行。
E2E_EXPOSE_MINIO_HOSTPORT="${E2E_EXPOSE_MINIO_HOSTPORT:-1}"
# 1 = Kafka EXTERNAL + hostPort 30902; bootstrap replaces __KAFKA_LAN_HOST__ in manifests/kafka-kraft.yaml .
# 远程客户端 bootstrap: <E2E_KAFKA_ADVERTISED_HOST>:30902（未设置则脚本会尽力探测本机 IPv4）。
E2E_EXPOSE_KAFKA_HOSTPORT="${E2E_EXPOSE_KAFKA_HOSTPORT:-1}"
E2E_KAFKA_ADVERTISED_HOST="${E2E_KAFKA_ADVERTISED_HOST:-}"
# 1 = MinIO uses hostPath on Kind node /mnt/yikv-e2e-minio (requires Kind extraMounts: bootstrap-kind.sh + kind-config-persist.yaml.in).
E2E_PERSIST_MINIO_DATA="${E2E_PERSIST_MINIO_DATA:-0}"
E2E_MINIO_HOST_DATA_DIR="${E2E_MINIO_HOST_DATA_DIR:-${HOME}/yikv-e2e-data/minio}"

_resolve_kafka_lan_host() {
  local ip=""
  if command -v ip >/dev/null 2>&1; then
    ip="$(ip -4 route get 8.8.8.8 2>/dev/null | awk '{for (i = 1; i <= NF; i++) if ($i == "src") { print $(i + 1); exit }}')"
  fi
  if [[ -z "${ip}" ]] && command -v hostname >/dev/null 2>&1; then
    ip="$(hostname -I 2>/dev/null | awk '{print $1}')"
  fi
  echo "${ip:-127.0.0.1}"
}

_need_cmd() {
  command -v "$1" >/dev/null 2>&1 || {
    echo "bootstrap-infra: need '$1' in PATH" >&2
    exit 1
  }
}

_need_cmd kubectl

# Kind/kubeconfig 里集群「名字还在」但 Docker 节点停了 / kubeconfig 仍是旧端口时，kubectl 会连 127.0.0.1:随机端口 被拒绝。
_ensure_kube_api_reachable() {
  local ctx hint_kind out
  ctx="$(kubectl config current-context 2>/dev/null || true)"
  hint_kind="yikv-e2e"
  if [[ "${ctx}" == kind-* ]]; then
    hint_kind="${ctx#kind-}"
  fi
  if kubectl cluster-info --request-timeout=15s >/dev/null 2>&1; then
    return 0
  fi
  out="$(kubectl cluster-info --request-timeout=5s 2>&1 || true)"
  echo "bootstrap-infra: 无法连接 Kubernetes API（当前 context: ${ctx:-<none>}）。" >&2
  echo "  常见于 Kind：本机还留着 cluster 记录，但 control-plane 容器未运行，或 kubeconfig 里 server 端口已过期。" >&2
  echo "" >&2
  echo "  可依次尝试：" >&2
  echo "    1) kind export kubeconfig --name ${hint_kind}" >&2
  echo "    2) 确认 Docker 已运行；勿对 kindest/node 使用 docker start（易触发 old IPv4 错误）。" >&2
  echo "    3) \"${SCRIPT_DIR}/bootstrap-kind.sh\"  # 存在但不可达时会自动删建；或 kind delete cluster --name ${hint_kind} 后重跑" >&2
  echo "" >&2
  echo "  kubectl 报错摘要: ${out}" >&2
  exit 1
}

_ensure_kube_api_reachable

_ensure_local_path() {
  if kubectl get storageclass local-path >/dev/null 2>&1; then
    echo "==> StorageClass local-path already present"
    return 0
  fi
  echo "==> Installing local-path provisioner (default StorageClass on many Kind setups)"
  kubectl apply --validate=false -f "${LOCAL_PATH_URL}" || {
    echo "bootstrap-infra: local-path install failed; set LOCAL_PATH_URL or install a default StorageClass" >&2
    exit 1
  }
  kubectl patch storageclass local-path -p '{"metadata": {"annotations":{"storageclass.kubernetes.io/is-default-class":"true"}}}' \
    2>/dev/null || true
}

_ensure_local_path

echo "==> Namespace ${MINIO_NAMESPACE}"
kubectl create namespace "${MINIO_NAMESPACE}" --dry-run=client -o yaml | kubectl apply -f -

_apply_minio() {
  local manifest="${YIKV_SERVER_ROOT}/deploy/scripts/minio-embedded.yaml"
  [[ -f "${manifest}" ]] || {
    echo "bootstrap-infra: missing ${manifest}" >&2
    exit 1
  }
  if [[ "${E2E_EXPOSE_MINIO_HOSTPORT}" != "1" && "${E2E_PERSIST_MINIO_DATA}" != "1" ]]; then
    MINIO_NAMESPACE="${MINIO_NAMESPACE}" bash "${YIKV_SERVER_ROOT}/deploy/scripts/apply_minio_embedded.sh"
    return 0
  fi
  MINIO_NS="${MINIO_NAMESPACE}" MANIFEST="${manifest}" E2E_EXPOSE_MINIO_HOSTPORT="${E2E_EXPOSE_MINIO_HOSTPORT}" \
    E2E_PERSIST_MINIO_DATA="${E2E_PERSIST_MINIO_DATA}" python3 <<'PY'
import os
import pathlib
import sys

ns = os.environ["MINIO_NS"]
path = pathlib.Path(os.environ["MANIFEST"])
text = path.read_text(encoding="utf-8").replace("NAMESPACE_PLACEHOLDER", ns)

if os.environ.get("E2E_PERSIST_MINIO_DATA") == "1":
    parts = text.split("\n---\n")
    keep = []
    for p in parts:
        body = p.lstrip("\n")
        lines = body.splitlines()
        kind_line = next((ln for ln in lines if ln.startswith("kind:")), "")
        if kind_line.strip() == "kind: PersistentVolumeClaim":
            continue
        keep.append(p)
    text = "\n---\n".join(keep)
    oldvol = (
        "      volumes:\n"
        "        - name: data\n"
        "          persistentVolumeClaim:\n"
        "            claimName: minio-data"
    )
    newvol = (
        "      volumes:\n"
        "        - name: data\n"
        "          hostPath:\n"
        "            path: /mnt/yikv-e2e-minio\n"
        "            type: DirectoryOrCreate"
    )
    if oldvol not in text:
        print("bootstrap-infra: could not switch MinIO to hostPath (volumes stanza changed?)", file=sys.stderr)
        sys.exit(1)
    text = text.replace(oldvol, newvol, 1)

if os.environ.get("E2E_EXPOSE_MINIO_HOSTPORT") == "1":
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
        print("bootstrap-infra: could not inject MinIO hostPorts (ports stanza changed?)", file=sys.stderr)
        sys.exit(1)
    text = text.replace(needle, repl, 1)

sys.stdout.write(text)
PY
}

echo "==> MinIO (embedded manifest)"
if [[ "${E2E_PERSIST_MINIO_DATA}" == "1" ]]; then
  echo "    (E2E_PERSIST_MINIO_DATA=1: hostPath /mnt/yikv-e2e-minio on Kind node ↔ host dir ${E2E_MINIO_HOST_DATA_DIR} via extraMounts)"
fi
if [[ "${E2E_EXPOSE_MINIO_HOSTPORT}" == "1" || "${E2E_PERSIST_MINIO_DATA}" == "1" ]]; then
  if [[ "${E2E_EXPOSE_MINIO_HOSTPORT}" == "1" ]]; then
    echo "    (E2E_EXPOSE_MINIO_HOSTPORT=1: S3/console on host TCP 30900/30901 for LAN; see README)"
  fi
  _apply_minio | kubectl apply -f -
else
  _apply_minio
fi

echo "==> Kafka (KRaft single broker)"
if [[ "${E2E_EXPOSE_KAFKA_HOSTPORT}" == "1" ]]; then
  _kafka_host="${E2E_KAFKA_ADVERTISED_HOST:-$(_resolve_kafka_lan_host)}"
  echo "    (E2E_EXPOSE_KAFKA_HOSTPORT=1: LAN bootstrap ${_kafka_host}:30902 ; override with E2E_KAFKA_ADVERTISED_HOST)"
  if [[ "${_kafka_host}" == "127.0.0.1" ]]; then
    echo "    WARN: advertised EXTERNAL is 127.0.0.1 — remote machines cannot use it; set E2E_KAFKA_ADVERTISED_HOST=<Kind宿主机局域网IP>" >&2
  fi
  sed "s|__KAFKA_LAN_HOST__|${_kafka_host}|g" "${SCRIPT_DIR}/manifests/kafka-kraft.yaml" | kubectl apply -f -
else
  echo "    (E2E_EXPOSE_KAFKA_HOSTPORT=0: in-cluster only; see manifests/kafka-kraft-internal.yaml)"
  kubectl apply -f "${SCRIPT_DIR}/manifests/kafka-kraft-internal.yaml"
fi

echo "==> Wait for MinIO + Kafka"
kubectl rollout status deployment/minio -n "${MINIO_NAMESPACE}" --timeout=180s
kubectl rollout status deployment/kafka -n "${MINIO_NAMESPACE}" --timeout=300s

echo "==> Ensure artifact bucket (port-forward + MinIO API)"
if ! python3 -c "import kubernetes, minio" 2>/dev/null; then
  echo ""
  echo "WARN: python kubernetes/minio not installed; skip automatic bucket create."
  echo "  pip install -r ${YIKV_SERVER_ROOT}/requirements.txt"
  if [[ "${E2E_EXPOSE_MINIO_HOSTPORT:-1}" == "1" ]]; then
    echo "  已暴露 hostPort 时可不 port-forward，直接:"
    echo "  python3 ${YIKV_SERVER_ROOT}/deploy/scripts/bootstrap_minio.py --namespace ${MINIO_NAMESPACE} \\"
    echo "    --bucket yikv-artifacts --bucket-only --minio-s3-host 127.0.0.1 --minio-s3-port 30900"
    echo "  MinIO LAN: http://<Kind宿主机>:30900 (S3) / :30901 (Console)"
  else
    echo "  Then in another terminal: kubectl port-forward -n ${MINIO_NAMESPACE} svc/minio 9000:9000"
    echo "  python3 ${YIKV_SERVER_ROOT}/deploy/scripts/bootstrap_minio.py --namespace ${MINIO_NAMESPACE} \\"
    echo "    --bucket yikv-artifacts --bucket-only --minio-s3-host 127.0.0.1 --minio-s3-port 9000"
  fi
  echo ""
  exit 0
fi

PF_PID=""
cleanup_pf() {
  if [[ -n "${PF_PID}" ]] && kill -0 "${PF_PID}" 2>/dev/null; then
    kill "${PF_PID}" 2>/dev/null || true
    wait "${PF_PID}" 2>/dev/null || true
  fi
}
trap cleanup_pf EXIT

kubectl port-forward -n "${MINIO_NAMESPACE}" svc/minio 19000:9000 >/dev/null 2>&1 &
PF_PID=$!
sleep 2
python3 "${YIKV_SERVER_ROOT}/deploy/scripts/bootstrap_minio.py" \
  --namespace "${MINIO_NAMESPACE}" \
  --bucket yikv-artifacts \
  --bucket-only \
  --minio-s3-host 127.0.0.1 \
  --minio-s3-port 19000

echo "==> ok: MinIO svc/minio:9000, Kafka svc/kafka:9092 (namespace ${MINIO_NAMESPACE})"
if [[ "${E2E_EXPOSE_MINIO_HOSTPORT:-1}" == "1" ]]; then
  echo "    MinIO 远程/本机（无需 port-forward）: S3 API http://<Kind 宿主机 IP>:30900  Console http://<IP>:30901  (minio/minio12345)"
else
  echo "    S3/Console 仅集群内或: kubectl port-forward -n ${MINIO_NAMESPACE} svc/minio 9000:9000 9001:9001"
fi
if [[ "${E2E_EXPOSE_KAFKA_HOSTPORT:-1}" == "1" ]]; then
  echo "    Kafka 远程: bootstrap <E2E_KAFKA_ADVERTISED_HOST 或自动探测的宿主机 IP>:30902（与集群内 kafka:9092 并存）"
else
  echo "    Kafka 仅集群内: kafka:9092"
fi
echo "    yikv-server RPC 默认 ClusterIP；集群外 kubectl port-forward -n yikv svc/yikv-server 9000:9000 或加 NodePort"
