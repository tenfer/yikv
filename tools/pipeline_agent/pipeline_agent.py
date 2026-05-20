#!/usr/bin/env python3
"""HTTP agent for build machine + online machine.

Composite routes (for schedulers): POST /publishIndex (build + push), POST /deployIndex (pull +
switch-active + link server + reload). Granular: /buildIndex, /pushIndex, /pullIndex, /switchReloadIndex.

Deploy a dedicated process on the build host and one on the online host; paths (WORK, BUILD_DB, …) are
set via environment on each machine. Schedulers should call HTTP only — see tools/schedule_pipeline.py.

No authentication (use private network / VPC).

  PIPELINE_CONFIG   optional JSON path: merged on top of tools/pipeline_agent/pipeline.defaults.json (if that file exists)

Environment (override JSON — same names as export from pipeline_config.py --emit-shell):
  YIKV_ROOT          repo root (default: parent of tools/)
  WORK               default /data/yikvdb
  BUILD_DB           default $WORK/build_db
  SERVER_DB          default $WORK/server_db
  ARTIFACT_STORE     default $WORK/artifact_store (when auto-creating artifact-storage.json)
  ARTIFACT_KEY_PREFIX  default yikv-index
  ARTIFACT_ENV       default dev
  ARTIFACT_CONFIG    default $WORK/artifact-storage.json；若 $SERVER_CONFIG（默认 $WORK/config.server.json）
                       含 artifact_storage 且未设置本变量，则默认与 SERVER_CONFIG 为同一路径
  ADMIN_SOCKET       default $WORK/admin.sock (should match parent(db_path)/admin.sock when db_path is $WORK/server_db)
  SERVER_CONFIG      default $WORK/config.server.json — must exist (manual); see config.example.json
  YIKV_IMPORT_BIN    default $YIKV_ROOT/bazel-bin/yikv_import_pipeline
  YIKV_SERVER_BIN    default $YIKV_ROOT/bazel-bin/yikv_server
  SCHEMA_JSON        default $YIKV_ROOT/schema.json (buildIndex fallback)
  IMPORT_LISTEN      default 127.0.0.1:59999 (import pipeline config listen)
  SERVER_LISTEN      not written by agent; set listen in SERVER_CONFIG to match your RPC bind address
  AUTO_START_SERVER  default 1 — deployIndex / switchReloadIndex may start yikv_server if admin socket missing
                        (stale socket files left after a crash are removed after a failed connect probe)
  PIPELINE_ARENA_MAX_GB  import + server JSON (default 4; must match offline build or OpenIndex fails)
  PIPELINE_ARENA_SEG_GB  segment size in GB (default 1)
  IMPORT_IO_WORKERS     yikv_import_pipeline parallel readers (default 1; lower RAM)
  IMPORT_QUEUE_BATCHES  max RecordBatches in flight (default 8; lower RAM)
  HOST               default 0.0.0.0
  PORT               default 8787
  ADMIN_SOCKET_WAIT_SEC  max seconds to wait for yikv_server admin socket after auto-start (default 90)
  CLEANUP_BUILD_DB_AFTER_PUSH  if 1/true, publishIndex removes BUILD_DB/<table> after successful push
                                  (same as cleanup_build_db_after_push on the JSON body)
"""

from __future__ import annotations

import os
import threading
from typing import Any

from fastapi import FastAPI, HTTPException

import pipeline_ops
from pipeline_models import (
    BuildIndexBody,
    DeployIndexBody,
    PipelineError,
    PublishIndexBody,
    PullIndexBody,
    PushIndexBody,
    SwitchReloadBody,
)

CTX = pipeline_ops.PipelineContext.load()

_import_lock = threading.Lock()
app = FastAPI(title="yikv pipeline agent", version="1.0.0")


def _http(e: PipelineError) -> HTTPException:
    return HTTPException(status_code=e.status_code, detail=e.detail)


@app.get("/health")
def health() -> dict[str, str]:
    return {"status": "ok"}


@app.post("/publishIndex")
def publish_index(body: PublishIndexBody) -> dict[str, Any]:
    try:
        return pipeline_ops.publish_index(CTX, body, import_lock=_import_lock)
    except PipelineError as e:
        raise _http(e) from e


@app.post("/deployIndex")
def deploy_index(body: DeployIndexBody) -> dict[str, Any]:
    try:
        return pipeline_ops.deploy_index(CTX, body)
    except PipelineError as e:
        raise _http(e) from e


@app.post("/buildIndex")
def build_index(body: BuildIndexBody) -> dict[str, Any]:
    try:
        with _import_lock:
            return pipeline_ops.build_index(CTX, body)
    except PipelineError as e:
        raise _http(e) from e


@app.post("/pushIndex")
def push_index(body: PushIndexBody) -> dict[str, Any]:
    try:
        return pipeline_ops.push_index(CTX, body)
    except PipelineError as e:
        raise _http(e) from e


@app.post("/pullIndex")
def pull_index(body: PullIndexBody) -> dict[str, Any]:
    try:
        return pipeline_ops.pull_index(CTX, body)
    except PipelineError as e:
        raise _http(e) from e


@app.post("/switchReloadIndex")
def switch_reload_index(body: SwitchReloadBody) -> dict[str, Any]:
    try:
        return pipeline_ops.switch_reload_index(CTX, body)
    except PipelineError as e:
        raise _http(e) from e


def main() -> None:
    host = os.environ.get("HOST", "0.0.0.0")
    port = int(os.environ.get("PORT", "8787"))
    import uvicorn

    uvicorn.run(app, host=host, port=port, log_level="info")


if __name__ == "__main__":
    main()
