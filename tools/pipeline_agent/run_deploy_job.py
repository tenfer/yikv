#!/usr/bin/env python3
"""Kubernetes Job entrypoint: deploy (pull + link + reload) via pipeline_ops only.

Environment:
  DEPLOY_SPEC_JSON   Required. JSON object for DeployIndexBody (same fields as POST /deployIndex).

On success, prints one line of JSON to stdout (includes build_id) for kubectl logs.
"""

from __future__ import annotations

import json
import os
import sys


def main() -> int:
    raw = os.environ.get("DEPLOY_SPEC_JSON", "").strip()
    if not raw:
        print("run_deploy_job: DEPLOY_SPEC_JSON is required", file=sys.stderr)
        return 2
    from pipeline_models import DeployIndexBody
    from pipeline_ops import PipelineContext, PipelineError, deploy_index

    try:
        body = DeployIndexBody.model_validate_json(raw)
    except Exception as exc:
        print(f"run_deploy_job: invalid DEPLOY_SPEC_JSON: {exc}", file=sys.stderr)
        return 2
    ctx = PipelineContext.load()
    try:
        out = deploy_index(ctx, body)
    except PipelineError as exc:
        print(exc.detail, file=sys.stderr)
        return 1
    print(json.dumps(out, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
