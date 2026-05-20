#!/usr/bin/env bash
# WSL/Docker restart recovery: wait for Docker, then recreate Kind + infra if needed and redeploy yikv-server.
#
# Usage (from yikv-server/deploy/e2e):
#   ./recover-e2e.sh
#
# Same env as bootstrap-kind.sh / bootstrap-infra / deploy-online, e.g.:
#   E2E_PERSIST_MINIO_DATA=1 E2E_KAFKA_ADVERTISED_HOST=192.168.1.10 ./recover-e2e.sh
#
# Wait for Docker (avoid racing Docker Desktop after WSL boot), then:
#   bootstrap-kind.sh -> deploy-online.sh

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# Max wait ~120s (60 * 2s).
DOCKER_WAIT_MAX="${DOCKER_WAIT_MAX:-60}"

_need_cmd() {
  command -v "$1" >/dev/null 2>&1 || {
    echo "recover-e2e: need '$1' in PATH" >&2
    exit 1
  }
}

_need_cmd docker

_wait_docker() {
  local n=0
  while ! docker info >/dev/null 2>&1; do
    n=$((n + 1))
    if [[ "${n}" -ge "${DOCKER_WAIT_MAX}" ]]; then
      echo "recover-e2e: Docker not reachable after ${DOCKER_WAIT_MAX} attempts (~$((DOCKER_WAIT_MAX * 2))s)." >&2
      echo "  Start Docker Desktop / dockerd, then retry." >&2
      exit 1
    fi
    if [[ "${n}" -eq 1 ]] || [[ $((n % 15)) -eq 0 ]]; then
      echo "recover-e2e: waiting for Docker (attempt ${n}/${DOCKER_WAIT_MAX})..."
    fi
    sleep 2
  done
}

echo "==> recover-e2e: ensure Docker is up"
_wait_docker

echo "==> recover-e2e: bootstrap Kind + MinIO + Kafka"
"${SCRIPT_DIR}/bootstrap-kind.sh"

echo "==> recover-e2e: deploy yikv-server (e2e overlay)"
"${SCRIPT_DIR}/deploy-online.sh"

echo ""
echo "recover-e2e: done. Next: e.g. run-publish-job.sh with your publish spec."
