#!/usr/bin/env bash
# Start pipeline_agent and optionally yikv_server.
#
# Run with: ./start.sh or bash start.sh (do not use `sh start.sh` on Ubuntu — sh is dash).
# If invoked as `sh start.sh`, we re-exec under bash automatically.
#
# YIKV_PIPELINE_ROLE (required):
#   build  — only HTTP agent (publish / buildIndex / pushIndex)
#   online — yikv_server (background) + HTTP agent (deploy / pull / reload)
#   both   — same as online (build + online on one host)
#
# Common env: YIKV_ROOT, PIPELINE_CONFIG (optional JSON override), WORK, SERVER_CONFIG, …
# yikv_server defaults admin_unix_socket to {parent of db_path}/admin.sock when omitted (use
# db_path …/server_db and WORK …/ so this matches $WORK/admin.sock).
# ADMIN_SOCKET_WAIT_SEC  default 90 — max wait for admin socket after starting yikv_server
# (defaults match pipeline_agent.py). Install deps: pip install -r requirements.txt
#
if [ -z "${BASH_VERSION:-}" ]; then
  exec /usr/bin/env bash "$0" "$@"
fi
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
YIKV_ROOT="${YIKV_ROOT:-$SCRIPT_DIR}"
export YIKV_ROOT
AGENT_PY="${AGENT_PY:-$SCRIPT_DIR/tools/pipeline_agent/pipeline_agent.py}"

ROLE="${YIKV_PIPELINE_ROLE:-}"
if [[ -z "$ROLE" ]]; then
  echo "start.sh: set YIKV_PIPELINE_ROLE=build|online|both" >&2
  exit 1
fi
ROLE_LC="$(printf '%s' "$ROLE" | tr '[:upper:]' '[:lower:]')"

WORK="${WORK:-/data/yikvdb}"
ADMIN_SOCKET="${ADMIN_SOCKET:-$WORK/admin.sock}"
SERVER_CONFIG="${SERVER_CONFIG:-$WORK/config.server.json}"
YIKV_SERVER_BIN="${YIKV_SERVER_BIN:-$YIKV_ROOT/bazel-bin/yikv_server}"

need_yikv=0
case "$ROLE_LC" in
  build) need_yikv=0 ;;
  online | both) need_yikv=1 ;;
  *)
    echo "start.sh: YIKV_PIPELINE_ROLE must be build, online, or both (got: $ROLE)" >&2
    exit 1
    ;;
esac

admin_unix_reachable() {
  python3 -c "
import socket, sys
path = sys.argv[1]
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.settimeout(2.0)
try:
    s.connect(path)
except OSError:
    sys.exit(1)
else:
    sys.exit(0)
finally:
    s.close()
" "$ADMIN_SOCKET"
}

start_yikv_server() {
  mkdir -p "$WORK"
  if [[ ! -f "$SERVER_CONFIG" ]]; then
    echo "start.sh: WARNING: missing $SERVER_CONFIG — yikv_server not started." >&2
    echo "start.sh: Create server JSON (e.g. after first deployIndex link step) or copy from config.example.json" >&2
    return 0
  fi
  if [[ ! -x "$YIKV_SERVER_BIN" ]]; then
    echo "start.sh: building yikv_server..." >&2
    (cd "$YIKV_ROOT" && bazel build //:yikv_server)
  fi
  [[ -x "$YIKV_SERVER_BIN" ]] || { echo "start.sh: not executable: $YIKV_SERVER_BIN" >&2; exit 1; }

  if [[ -S "$ADMIN_SOCKET" ]] && admin_unix_reachable; then
    echo "start.sh: admin socket already accepting connections: $ADMIN_SOCKET (skip yikv_server)" >&2
    return 0
  fi
  if [[ -e "$ADMIN_SOCKET" ]]; then
    echo "start.sh: removing stale or unusable admin socket: $ADMIN_SOCKET" >&2
    rm -f "$ADMIN_SOCKET"
  fi

  echo "start.sh: starting yikv_server -> $YIKV_SERVER_BIN $SERVER_CONFIG" >&2
  nohup "$YIKV_SERVER_BIN" "$SERVER_CONFIG" >>"$WORK/yikv_server.log" 2>&1 &
  echo $! >"$WORK/yikv_server.pid"

  local wait_sec="${ADMIN_SOCKET_WAIT_SEC:-90}"
  local max_n=$((wait_sec * 5))
  local n=0
  local pid
  pid="$(<"$WORK/yikv_server.pid")"

  while true; do
    if admin_unix_reachable; then
      break
    fi
    if ! kill -0 "$pid" 2>/dev/null; then
      echo "start.sh: yikv_server (pid $pid) exited before admin socket was ready." >&2
      echo "start.sh: --- tail of $WORK/yikv_server.log ---" >&2
      tail -n 120 "$WORK/yikv_server.log" 2>/dev/null || true
      exit 1
    fi
    sleep 0.2
    n=$((n + 1))
    if [[ $n -gt max_n ]]; then
      echo "start.sh: yikv_server did not accept connections on $ADMIN_SOCKET within ${wait_sec}s (set ADMIN_SOCKET_WAIT_SEC)." >&2
      echo "start.sh: Ensure $SERVER_CONFIG contains admin_unix_socket and see $WORK/yikv_server.log" >&2
      tail -n 120 "$WORK/yikv_server.log" 2>/dev/null || true
      exit 1
    fi
  done
  echo "start.sh: yikv_server up (pid $(<"$WORK/yikv_server.pid"))" >&2
}

if [[ "$need_yikv" -eq 1 ]]; then
  start_yikv_server
fi

cd "$YIKV_ROOT"
echo "start.sh: starting pipeline_agent (YIKV_PIPELINE_ROLE=$ROLE_LC) ..." >&2
exec python3 "$AGENT_PY"
