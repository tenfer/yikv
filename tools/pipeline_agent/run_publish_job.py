#!/usr/bin/env python3
"""Kubernetes Job entrypoint: publish (build + push) via pipeline_ops only.

Environment:
  PUBLISH_SPEC_JSON   Required. JSON object for PublishIndexBody (same fields as POST /publishIndex).

Optional:
  PIPELINE_REMOTE_HEALTH_URL   After a successful publish, GET /health on this base URL
                               (remote pipeline_agent or monitor), using YikvPipelineClient.

On success, prints one line of JSON to stdout (includes build_id) for kubectl logs.
"""

from __future__ import annotations

import json
import os
import sys


def main() -> int:
    raw = os.environ.get("PUBLISH_SPEC_JSON", "").strip()
    if not raw:
        print("run_publish_job: PUBLISH_SPEC_JSON is required", file=sys.stderr)
        return 2
    from pipeline_http_client import YikvPipelineClient
    from pipeline_models import PublishIndexBody
    from pipeline_ops import PipelineContext, PipelineError, publish_index

    try:
        body = PublishIndexBody.model_validate_json(raw)
    except Exception as exc:
        print(f"run_publish_job: invalid PUBLISH_SPEC_JSON: {exc}", file=sys.stderr)
        return 2
    ctx = PipelineContext.load()
    try:
        out = publish_index(ctx, body, import_lock=None)
    except PipelineError as exc:
        print(exc.detail, file=sys.stderr)
        return 1
    # One-line JSON for operators: kubectl logs job/... | tail -1
    print(json.dumps(out, ensure_ascii=False))
    monitor = os.environ.get("PIPELINE_REMOTE_HEALTH_URL", "").strip()
    if monitor:
        try:
            c = YikvPipelineClient(monitor, monitor)
            c.health()
        except Exception as exc:
            print(f"run_publish_job: remote health check failed: {exc}", file=sys.stderr)
            return 3
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
