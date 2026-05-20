# Omit explicit `# syntax=docker/dockerfile:1` so BuildKit uses its bundled Dockerfile frontend.
# (A pinned syntax pulls docker.io/docker/dockerfile:1 first — fails when Docker Hub is unreachable.)
# If you need the upstream frontend, set the first line to a reachable mirror, e.g.:
#   # syntax=docker.m.daocloud.io/docker/dockerfile:1
# Build from yikv-server repository root (must include libs/yikv/ engine tree):
#   docker build -f deploy/docker/Dockerfile -t yikv-server:latest .   # final stage = runtime (yikv_server)
#   docker build -f deploy/docker/Dockerfile --target pipeline-worker -t pipeline-worker:latest .
#
# Bazel resolves MODULE deps online inside the image (no vendor/ snapshot); BuildKit cache at
# /root/.cache/bazel speeds repeat builds.
#
# If docker.io is unreachable, override base image, e.g.:
#   --build-arg UBUNTU_IMAGE=docker.m.daocloud.io/library/ubuntu:22.04
#
# If github.com is slow/unreachable, set a *prefix* that ends right before "github.com/…"
# ( ghproxy example: https://ghproxy.net/https://  then we append github.com/bazelbuild/… )
#   --build-arg BAZEL_DOWNLOAD_PREFIX=https://ghproxy.net/https://
# Or set an explicit full URL (wins):
#   --build-arg BAZEL_URL=https://…/bazel-7.4.1-linux-x86_64
#
# MODULE/http_archive (rules_cc, rules_foreign_cc, …):
#   Default: direct https://github.com/… (no rewrite). Prefer HTTP/HTTPS_PROXY if unreachable.
#   Optional rewrite via ghproxy (public nodes often 502 — use only if stable for you):
#   --build-arg USE_BAZEL_GITHUB_MIRROR=1
#
# Arrow C++ is built from source with S3 (install-arrow-s3.sh); Ubuntu libarrow-dev ships ARROW_S3=OFF.
# Toolchain: gcc-14/g++-14 (Ubuntu 24.04+ / 26.04; jammy 22.04 may lack gcc-14 — use noble or newer).
# For older bases (e.g. 24.04) you must pass a different UBUNTU_IMAGE and add Apache’s APT yourself.

ARG UBUNTU_IMAGE=ubuntu:26.04

FROM ${UBUNTU_IMAGE} AS builder-base
ENV DEBIAN_FRONTEND=noninteractive

RUN set -eux; \
    apt-get update; \
    apt-get install -y --no-install-recommends \
      build-essential \
      ca-certificates \
      curl \
      g++-14 \
      gcc-14 \
      git \
      libflatbuffers-dev \
      libmysqlclient-dev \
      libssl-dev \
      librdkafka-dev \
      nlohmann-json3-dev \
      pkg-config \
      rclone \
      unzip \
      zip \
    ; \
    update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-14 100 \
      --slave /usr/bin/g++ g++ /usr/bin/g++-14; \
    update-alternatives --set gcc /usr/bin/gcc-14; \
    if [ -e /usr/bin/cc ]; then \
      update-alternatives --install /usr/bin/cc cc /usr/bin/gcc-14 100 || true; \
      update-alternatives --set cc /usr/bin/gcc-14 || true; \
    fi; \
    gcc --version; \
    g++ --version; \
    rm -rf /var/lib/apt/lists/*

ENV CC=gcc-14
ENV CXX=g++-14

COPY deploy/docker/install-arrow-s3.sh /tmp/install-arrow-s3.sh
RUN chmod +x /tmp/install-arrow-s3.sh && /tmp/install-arrow-s3.sh && rm -f /tmp/install-arrow-s3.sh
RUN ldconfig /usr/local/lib
ENV LD_LIBRARY_PATH=/usr/local/lib
ENV PKG_CONFIG_PATH=/usr/local/lib/pkgconfig
RUN printf '%s\n' \
  'build --action_env=CC=gcc-14' \
  'build --action_env=CXX=g++-14' \
  'build --host_action_env=CC=gcc-14' \
  'build --host_action_env=CXX=g++-14' \
  'build --linkopt=-L/usr/local/lib' \
  'build --linkopt=-Wl,-rpath,/usr/local/lib' \
  'build --copt=-DARROW_S3' \
  >> /root/.bazelrc

ARG BAZEL_VERSION=7.4.1
ARG BAZEL_DOWNLOAD_PREFIX=
ARG BAZEL_URL=
RUN set -eux; \
    default_path="github.com/bazelbuild/bazel/releases/download/${BAZEL_VERSION}/bazel-${BAZEL_VERSION}-linux-x86_64"; \
    if [ -n "${BAZEL_URL}" ]; then \
      url="${BAZEL_URL}"; \
    elif [ -n "${BAZEL_DOWNLOAD_PREFIX}" ]; then \
      url="${BAZEL_DOWNLOAD_PREFIX}${default_path}"; \
    else \
      url="https://${default_path}"; \
    fi; \
    echo "Fetching bazel from: ${url}"; \
    curl -fSL --connect-timeout 30 --retry 5 --retry-delay 3 --max-time 900 "${url}" \
      -o /usr/local/bin/bazel; \
    chmod +x /usr/local/bin/bazel

# Optional: rewrite GitHub fetch URLs through ghproxy (same rules as deploy/docker/bazel-downloader-ghproxy.cfg).
# Default 0 — ghproxy.net is unreliable (502); use HTTP_PROXY/HTTPS_PROXY or a corporate mirror instead.
# Inlined so `docker build` does not COPY from context (avoids missing file when context/clone omits it).
RUN mkdir -p /etc/bazel && printf '%s\n' \
  '# Bazel --experimental_downloader_config (ghproxy); keep in sync with deploy/docker/bazel-downloader-ghproxy.cfg' \
  'rewrite (github.com)/(.*) https://ghproxy.net/https://$1/$2' \
  'rewrite (raw.githubusercontent.com)/(.*) https://ghproxy.net/https://$1/$2' \
  'rewrite (objects.githubusercontent.com)/(.*) https://ghproxy.net/https://$1/$2' \
  > /etc/bazel/downloader.cfg
ARG USE_BAZEL_GITHUB_MIRROR=0
RUN if [ "${USE_BAZEL_GITHUB_MIRROR}" = "1" ]; then \
      printf '%s\n' \
        'common --experimental_downloader_config=/etc/bazel/downloader.cfg' \
        >> /root/.bazelrc; \
    fi

WORKDIR /src


# Online Bzlmod — same default as host `bazel build` (repo `.bazelrc` + `bazel/vendor.bazelrc`; no `--vendor_dir`).
FROM builder-base AS builder
ARG HTTP_PROXY=
ARG HTTPS_PROXY=
ARG NO_PROXY=
ENV HTTP_PROXY=${HTTP_PROXY} HTTPS_PROXY=${HTTPS_PROXY} NO_PROXY=${NO_PROXY}

COPY . /src/yikv-server
WORKDIR /src/yikv-server

# Optional: e.g. `cn` — ghproxy downloader + tuning (see repo `.bazelrc`). Name kept for CI scripts.
ARG BAZEL_VENDOR_CONFIG=
RUN if [ -n "${BAZEL_VENDOR_CONFIG}" ]; then \
      printf '%s\n' "common --config=${BAZEL_VENDOR_CONFIG}" >> /root/.bazelrc; \
      echo "bazel: common --config=${BAZEL_VENDOR_CONFIG}"; \
    fi

# Resource limits (WSL / low-RAM hosts): pass at build time, e.g.
#   --build-arg BAZEL_JOBS=2
#   --build-arg BAZEL_LOCAL_RAM_RESOURCES=4096
# Or set env from run-publish-job.sh (see deploy/e2e/README.md).

ARG BAZEL_JOBS=
ARG BAZEL_LOCAL_RAM_RESOURCES=
# third_party stubs are minimal; compilers pick full headers from these packages (same as typical dev machines).
# --verbose_failures: surface full gcc stderr when Abseil/third_party compile fails (OOM, -std, etc.).
RUN --mount=type=cache,target=/root/.cache/bazel,sharing=locked \
    set -eux; \
    if [ -n "${BAZEL_JOBS}" ]; then \
      printf '%s\n' "build --jobs=${BAZEL_JOBS}" >> /root/.bazelrc; \
      echo "bazel: build --jobs=${BAZEL_JOBS} (from build-arg BAZEL_JOBS)"; \
    fi; \
    if [ -n "${BAZEL_LOCAL_RAM_RESOURCES}" ]; then \
      printf '%s\n' "build --local_ram_resources=${BAZEL_LOCAL_RAM_RESOURCES}" >> /root/.bazelrc; \
      echo "bazel: build --local_ram_resources=${BAZEL_LOCAL_RAM_RESOURCES}"; \
    fi; \
    bazel build -c opt --verbose_failures //:yikv_server //:yikv_import_pipeline; \
    mkdir -p /opt/yikv-bins; \
    imp_rel="$(bazel cquery --output=files -c opt //:yikv_import_pipeline | head -n1)"; \
    srv_rel="$(bazel cquery --output=files -c opt //:yikv_server | head -n1)"; \
    test -n "${imp_rel}" && test -n "${srv_rel}"; \
    test -f "${imp_rel}" && test -f "${srv_rel}"; \
    cp -a "${imp_rel}" /opt/yikv-bins/yikv_import_pipeline; \
    cp -a "${srv_rel}" /opt/yikv-bins/yikv_server


# -----------------------------------------------------------------------------
# pipeline-worker: in-cluster publish (import + artifact_sync + pipeline_ops).
# No FastAPI/uvicorn — entrypoint is tools/pipeline_agent/run_publish_job.py.
# Build: docker build -f deploy/docker/Dockerfile --target pipeline-worker -t pipeline-worker:latest .
# -----------------------------------------------------------------------------
FROM ${UBUNTU_IMAGE} AS pipeline-worker
ENV DEBIAN_FRONTEND=noninteractive

RUN set -eux; \
    apt-get update; \
    libmysql_rt="$(apt-cache pkgnames libmysqlclient | grep -E '^libmysqlclient[0-9]+$' | sort -V | tail -n1)"; \
    test -n "${libmysql_rt}"; \
    apt-get install -y --no-install-recommends \
      ca-certificates \
      curl \
      libgcc-s1 \
      liblz4-1 \
      "${libmysql_rt}" \
      librdkafka1 \
      libsasl2-2 \
      libssl3 \
      libstdc++6 \
      python3 \
      python3-venv \
      rclone \
      zlib1g \
      libzstd1 \
    ; \
    apt-get autoremove -y; \
    rm -rf /var/lib/apt/lists/*

COPY --from=builder /usr/local/lib/libarrow.so* /usr/local/lib/
COPY --from=builder /usr/local/lib/libparquet.so* /usr/local/lib/
RUN ldconfig /usr/local/lib

COPY --from=builder /opt/yikv-bins/yikv_import_pipeline /usr/local/bin/yikv_import_pipeline
RUN chmod +x /usr/local/bin/yikv_import_pipeline

ENV LD_LIBRARY_PATH=/usr/local/lib

# Slim checkout: avoid copying vendor/ & bazel outputs from BuildKit context (~GB).
COPY --from=builder /src/yikv-server/requirements.txt /opt/yikv/yikv-server/requirements.txt
COPY --from=builder /src/yikv-server/config.default.json /opt/yikv/yikv-server/config.default.json
COPY --from=builder /src/yikv-server/tools /opt/yikv/yikv-server/tools
COPY --from=builder /src/yikv-server/deploy /opt/yikv/yikv-server/deploy

WORKDIR /opt/yikv/yikv-server/tools/pipeline_agent
ENV YIKV_ROOT=/opt/yikv/yikv-server
ENV YIKV_IMPORT_BIN=/usr/local/bin/yikv_import_pipeline
ENV PYTHONPATH=/opt/yikv/yikv-server/tools/pipeline_agent
ENV PATH=/opt/yikv-venv/bin:${PATH}

RUN python3 -m venv /opt/yikv-venv \
    && /opt/yikv-venv/bin/pip install --no-cache-dir -r /opt/yikv/yikv-server/requirements.txt

CMD ["python3", "run_publish_job.py"]

# Default final stage for plain `docker build`: online yikv_server image.
FROM ${UBUNTU_IMAGE} AS runtime
ENV DEBIAN_FRONTEND=noninteractive

# yikv_server: C++ only, mmap local db_path — no embedded S3 client for published artifacts.
# Pull from S3-compatible storage uses tools/artifact_sync (Python stdlib) + rclone (same as
# pipeline_agent / pipeline-worker). InitContainer or an overridden command can pull+switch
# then exec yikv_server (see deploy/docker/README.md).
RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates \
    libgcc-s1 \
    liblz4-1 \
    librdkafka1 \
    libsasl2-2 \
    libssl3 \
    libstdc++6 \
    python3-minimal \
    rclone \
    zlib1g \
    libzstd1 \
    && rm -rf /var/lib/apt/lists/*

COPY --from=builder /opt/yikv-bins/yikv_server /usr/local/bin/yikv_server
RUN chmod +x /usr/local/bin/yikv_server

# Default server JSON at the path the binary uses when no CLI arg / YIKV_SERVER_CONFIG.
# Kubernetes: replace with a volumeMount on /etc/yikv (ConfigMap key config.json), which hides this file.
COPY --from=builder /src/yikv-server/config.default.json /etc/yikv/config.json

# Stdlib-only; top-level artifact_storage in JSON is ignored by yikv_server but read by -c config.
COPY --from=builder /src/yikv-server/tools/artifact_sync /opt/yikv/artifact_sync
RUN chmod +x /opt/yikv/artifact_sync/artifact_sync.py

ENV YIKV_ARTIFACT_SYNC=/opt/yikv/artifact_sync/artifact_sync.py

# No CMD: binary defaults to /etc/yikv/config.json (mount ConfigMap/volume there).
ENTRYPOINT ["/usr/local/bin/yikv_server"]
