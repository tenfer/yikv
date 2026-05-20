#!/usr/bin/env bash
# Sync canonical yikv engine sources into this monorepo (libs/yikv/).
#
# Prerequisites: sibling clone by default (${REPO_ROOT}/../yikv) or override:
#   YIKV_SOURCE=/path/to/yikv ./scripts/sync_yikv_lib.sh
#
# Excludes MODULE.bzlmod-era artifacts; removes libs/yikv/MODULE.bazel after sync
# (root MODULE.bazel wires BCR + brpc tarball for the whole workspace).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC="${YIKV_SOURCE:-$(cd "${ROOT}/.." && pwd)/yikv}"
DEST="${ROOT}/libs/yikv"
[[ -f "${SRC}/src/db/db.cc" ]] || {
  echo "sync_yikv_lib: invalid YIKV_SOURCE (missing ${SRC}/src/db/db.cc): ${SRC}" >&2
  exit 1
}
mkdir -p "${DEST}"
rsync -a --delete \
  --exclude=.git \
  --exclude=bazel-* \
  --exclude=bazel-bin \
  --exclude=bazel-out \
  --exclude=bazel-testlogs \
  --exclude=vendor \
  "${SRC}/" "${DEST}/"
rm -f "${DEST}/MODULE.bazel"
# Upstream yikv keeps engine under src/; this monorepo uses libs/yikv/{alloc,container,db,index,schema}.
if [[ -d "${DEST}/src/alloc" ]]; then
  for d in alloc container db index schema; do
    [[ -d "${DEST}/src/${d}" ]] || continue
    rm -rf "${DEST}/${d}"
    mv "${DEST}/src/${d}" "${DEST}/"
  done
  rmdir "${DEST}/src" 2>/dev/null || {
    echo "sync_yikv_lib: warning: ${DEST}/src not empty after flatten; remove or merge manually" >&2
  }
fi
# Upstream headers/sources use #include "src/..."; monorepo resolves from libs/yikv via -Ilibs/yikv.
for sub in alloc container db index schema; do
  [[ -d "${DEST}/${sub}" ]] || continue
  while IFS= read -r -d '' f; do
    sed -i 's|#include "src/|#include "|g' "$f"
  done < <(find "${DEST}/${sub}" -type f \( -name '*.cc' -o -name '*.h' \) -print0)
done
echo "sync_yikv_lib: ${SRC}/ -> ${DEST}/ (removed MODULE.bazel for monolith Bzlmod)"
