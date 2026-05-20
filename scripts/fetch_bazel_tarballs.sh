#!/usr/bin/env bash
# Download tarballs under third_party/tarball for:
#   - local_tarball repos (nlohmann_json, brpc) in MODULE.bazel
#   - --distdir (bazel_features, rules_foreign_cc CMake, libpfm, …) via bazel/vendor.bazelrc
# Requires: curl, shasum or sha256sum, awk
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEST="${ROOT}/third_party/tarball"
mkdir -p "${DEST}"

die() { echo "fetch_bazel_tarballs: $*" >&2; exit 1; }

verify_sha256() {
  local file="$1" expected="$2"
  if command -v sha256sum >/dev/null 2>&1; then
    echo "${expected}  ${file}" | sha256sum -c -
  else
    echo "${expected}  ${file}" | shasum -a 256 -c -
  fi
}

fetch_one() {
  local name="$1" url="$2" sha="$3"
  local out="${DEST}/${name}"
  if [[ -f "${out}" ]]; then
    echo "OK (cached) ${name}"
    verify_sha256 "${out}" "${sha}" || die "checksum mismatch for ${name}; rm ${out} && retry"
    return
  fi
  echo "Fetching ${name} ..."
  curl -fL --retry 5 --connect-timeout 30 -o "${out}.part" "${url}" || die "curl failed: ${url}"
  verify_sha256 "${out}.part" "${sha}" || die "checksum mismatch for ${name}"
  mv "${out}.part" "${out}"
  echo "OK ${name}"
}

# nlohmann_json (strip_prefix json-3.10.5)
fetch_one "nlohmann_json-v3.10.5.tar.gz" \
  "https://github.com/nlohmann/json/archive/refs/tags/v3.10.5.tar.gz" \
  "5daca6ca216495edf89d167f808d1d03c4a4d929cef7da5e10f135ae1540c7e4"

# brpc (strip_prefix brpc-1.16.0)
fetch_one "brpc-1.16.0.tar.gz" \
  "https://github.com/apache/brpc/archive/refs/tags/1.16.0.tar.gz" \
  "60f218554527f05ad8fae3cb8f81879d0c7dc72b249cde132049c44b1a73e76d"

# bazel_features (basename must match archive_override URL for --distdir)
fetch_one "bazel_features-v1.28.0.tar.gz" \
  "https://github.com/bazel-contrib/bazel_features/releases/download/v1.28.0/bazel_features-v1.28.0.tar.gz" \
  "2f057dd02098a106095ea291b4344257398a059eadb2c74cc470de0f9664dccd"

# libpfm (BCR archive_override 首条 URL 的 basename；与 netcologne 路径一致便于 distdir)
fetch_one "libpfm-4.11.0.tar.gz" \
  "https://downloads.sourceforge.net/project/perfmon2/libpfm4/libpfm-4.11.0.tar.gz" \
  "5da5f8872bde14b3634c9688d980f68bda28b510268723cc12973eedbab9fecc"

# CMake linux x86_64 — checksum from official SHA256 manifest (basename matches rules_foreign_cc URL)
CMAKE_VER="3.23.2"
CMAKE_BASE="cmake-${CMAKE_VER}-linux-x86_64.tar.gz"
CMAKE_SHA_URL="https://github.com/Kitware/CMake/releases/download/v${CMAKE_VER}/cmake-${CMAKE_VER}-SHA-256.txt"
TMP_SHA="${DEST}/.cmake-sha256-${CMAKE_VER}.txt"
curl -fL --retry 5 --connect-timeout 30 -o "${TMP_SHA}" "${CMAKE_SHA_URL}" || die "curl failed: ${CMAKE_SHA_URL}"
CMAKE_EXPECTED="$(awk -v b="${CMAKE_BASE}" '$2 ~ b {print $1; exit}' "${TMP_SHA}")"
[[ -n "${CMAKE_EXPECTED}" ]] || die "could not find SHA256 for ${CMAKE_BASE} in manifest"
rm -f "${TMP_SHA}"
fetch_one "${CMAKE_BASE}" \
  "https://github.com/Kitware/CMake/releases/download/v${CMAKE_VER}/${CMAKE_BASE}" \
  "${CMAKE_EXPECTED}"

echo "All tarballs present under ${DEST}"
