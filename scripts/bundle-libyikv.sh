#!/usr/bin/env bash
# Merge Bazel-produced src/** lib*.pic.a archives into libyikv.a and libyikv.so
# under bazel-bin/ (POSIX-style install targets copy from there).

set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

MODE="${YIKV_COMPILATION_MODE:-opt}"

# shellcheck disable=SC2086
bazel build -c "${MODE}" ${EXTRA_BAZEL_FLAGS:-} ${BAZEL_FLAGS:-} \
  //src/db:db >/dev/null

BIN="$(bazel info -c "${MODE}" bazel-bin)"

mapfile -t LIBS < <(find "${BIN}/src" -type f -name 'lib*.pic.a' | LC_ALL=C sort -u)

if [[ ${#LIBS[@]} -eq 0 ]]; then
  echo "bundle-libyikv: no lib*.pic.a under ${BIN}/src" >&2
  exit 1
fi

TMP="$(mktemp -d)"
trap 'rm -rf "${TMP}"' EXIT

idx=0
for a in "${LIBS[@]}"; do
  d="${TMP}/d_${idx}"
  mkdir -p "${d}"
  (cd "${d}" && ar x "${a}")
  idx=$((idx + 1))
done

mkdir -p "${BIN}"

mapfile -t OBJS < <(find "${TMP}" -type f '(' -name '*.o' -o -name '*.pic.o' ')' \
  ! -path '*/CMakeFiles/*' | LC_ALL=C sort)

if [[ ${#OBJS[@]} -eq 0 ]]; then
  echo "bundle-libyikv: extracted no .o from archives" >&2
  exit 1
fi

OUT_A="${BIN}/libyikv.a"
OUT_SO="${BIN}/libyikv.so"

rm -f "${OUT_A}" "${OUT_SO}"
ar crs "${OUT_A}" "${OBJS[@]}"
ranlib "${OUT_A}" 2>/dev/null || true

CXX="${CXX:-g++}"
# shellcheck disable=SC2086
${CXX} -shared -fPIC -std=c++17 -pthread -Wl,-soname,libyikv.so \
  -o "${OUT_SO}" "${OBJS[@]}"

echo "bundle-libyikv: wrote ${OUT_A} ${OUT_SO}"
