#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OVERLAY="${OVERLAY:-$(cd "${SCRIPT_DIR}/overlays/prod" && pwd)}"
kubectl delete -k "${OVERLAY}" --ignore-not-found
