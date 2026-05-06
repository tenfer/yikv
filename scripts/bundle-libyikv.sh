#!/usr/bin/env bash
# Merge Bazel-produced src/** lib*.pic.a archives into libyikv.a and libyikv.so
# under bazel-bin/ (POSIX-style install targets copy from there).

set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

yikv_bazel() {
  if [[ "${YIKV_BAZEL_GHPROXY:-}" == 1 ]]; then
    bazel --noworkspace_rc \
      --bazelrc="$ROOT/bazel/registries/ghproxy.bazelrc" \
      --bazelrc="$ROOT/bazel/buildflags.bazelrc" "$@"
  else
    bazel "$@"
  fi
}

MODE="${YIKV_COMPILATION_MODE:-opt}"

# shellcheck disable=SC2086
yikv_bazel build -c "${MODE}" ${EXTRA_BAZEL_FLAGS:-} ${BAZEL_FLAGS:-} \
  //src/db:db >/dev/null

BIN="$(yikv_bazel info -c "${MODE}" bazel-bin)"

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

# Bazel sometimes emits PIC objects under _objs/ without a lib*.pic.a wrapper (e.g. kv_index).
# Pull in those .pic.o files when their basename is not already contributed by an archive.
declare -A seen_base
for o in "${OBJS[@]}"; do
  seen_base["$(basename "$o")"]=1
done
while IFS= read -r -d '' lone; do
  b="$(basename "${lone}")"
  [[ -n "${seen_base[${b}]:-}" ]] && continue
  seen_base["${b}"]=1
  OBJS+=("${lone}")
done < <(find "${BIN}/src" -path '*/_objs/*' -type f -name '*.pic.o' -print0 | LC_ALL=C sort -z)

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
