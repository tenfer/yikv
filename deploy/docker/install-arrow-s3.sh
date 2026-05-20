#!/usr/bin/env bash
# Build and install Apache Arrow C++ with Parquet + S3 into /usr/local.
# Ubuntu's libarrow-dev is compiled with ARROW_S3=OFF; yikv_import_pipeline needs S3 for s3:// inputs.
set -euo pipefail

# Use gcc-14 when present (Ubuntu 24.04+); default cc on 26.04 breaks aws-lc with -Werror.
if command -v gcc-14 >/dev/null 2>&1; then
  export CC=gcc-14
  export CXX=g++-14
fi

ARROW_VERSION="${ARROW_VERSION:-23.0.1}"
INSTALL_PREFIX="${INSTALL_PREFIX:-/usr/local}"
BUILD_DIR="${BUILD_DIR:-/tmp/arrow-cpp-build}"
SRC_TAR="/tmp/apache-arrow-${ARROW_VERSION}.tar.gz"

apt-get update
apt-get install -y --no-install-recommends \
  ca-certificates \
  cmake \
  curl \
  g++ \
  libbrotli-dev \
  libbz2-dev \
  libcurl4-openssl-dev \
  libgflags-dev \
  libgoogle-glog-dev \
  liblz4-dev \
  libre2-dev \
  libsnappy-dev \
  libssl-dev \
  libutf8proc-dev \
  libzstd-dev \
  ninja-build \
  pkg-config \
  rapidjson-dev \
  zlib1g-dev

if [[ ! -f "${SRC_TAR}" ]]; then
  curl -fSL --retry 5 --retry-delay 3 \
    "https://archive.apache.org/dist/arrow/arrow-${ARROW_VERSION}/apache-arrow-${ARROW_VERSION}.tar.gz" \
    -o "${SRC_TAR}"
fi

rm -rf "/tmp/apache-arrow-${ARROW_VERSION}"
tar -xzf "${SRC_TAR}" -C /tmp
cd "/tmp/apache-arrow-${ARROW_VERSION}/cpp"

rm -rf "${BUILD_DIR}"
cmake -S . -B "${BUILD_DIR}" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="${INSTALL_PREFIX}" \
  -DCMAKE_C_FLAGS="-Wno-error=discarded-qualifiers" \
  -DARROW_BUILD_SHARED=ON \
  -DARROW_BUILD_STATIC=OFF \
  -DARROW_BUILD_TESTS=OFF \
  -DARROW_BUILD_EXAMPLES=OFF \
  -DARROW_PARQUET=ON \
  -DARROW_S3=ON \
  -DARROW_WITH_BROTLI=ON \
  -DARROW_WITH_BZ2=ON \
  -DARROW_WITH_LZ4=ON \
  -DARROW_WITH_SNAPPY=ON \
  -DARROW_WITH_ZLIB=ON \
  -DARROW_WITH_ZSTD=ON

_jobs="$(nproc)"
if [[ "${_jobs}" -gt 8 ]]; then _jobs=8; fi
cmake --build "${BUILD_DIR}" --target install -j"${_jobs}"
ldconfig "${INSTALL_PREFIX}/lib" || true

if ! grep -q '#define ARROW_S3' "${INSTALL_PREFIX}/include/arrow/util/config.h"; then
  echo "install-arrow-s3: Arrow headers lack ARROW_S3" >&2
  exit 1
fi
echo "install-arrow-s3: Arrow ${ARROW_VERSION} with S3 installed to ${INSTALL_PREFIX}"
