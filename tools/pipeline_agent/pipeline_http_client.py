#!/usr/bin/env python3
"""Thin HTTP client for pipeline_agent: publishIndex / deployIndex / health.

Uses stdlib only (urllib). Does not import schedule_pipeline.

Example:
  from pipeline_http_client import YikvPipelineClient
  c = YikvPipelineClient("http://build:8787", "http://online:8787")
  print(c.publish_index(table="t1", input="/data/raw/t1"))
  print(c.deploy_index(table="t1"))
"""

from __future__ import annotations

import json
import os
import urllib.error
import urllib.request
from typing import Any


class YikvPipelineClient:
    def __init__(
        self,
        build_base_url: str,
        online_base_url: str | None = None,
        *,
        timeout_sec: float = 86400.0,
    ) -> None:
        self.build_base_url = build_base_url.rstrip("/")
        self.online_base_url = (online_base_url or build_base_url).rstrip("/")
        self.timeout_sec = timeout_sec

    def _post_json(self, base: str, path: str, body: dict[str, Any]) -> dict[str, Any]:
        url = base + path
        data = json.dumps(body, ensure_ascii=False).encode("utf-8")
        req = urllib.request.Request(
            url,
            data=data,
            method="POST",
            headers={"Content-Type": "application/json", "Accept": "application/json"},
        )
        try:
            with urllib.request.urlopen(req, timeout=self.timeout_sec) as resp:
                out = resp.read().decode("utf-8")
                if not out.strip():
                    return {}
                return json.loads(out)
        except urllib.error.HTTPError as e:
            raw = e.read().decode("utf-8", errors="replace")
            try:
                detail = json.loads(raw)
            except json.JSONDecodeError:
                detail = raw
            raise RuntimeError(f"HTTP {e.code} {url}: {detail}") from None

    def _get_json(self, base: str, path: str) -> dict[str, Any]:
        url = base + path
        req = urllib.request.Request(url, method="GET", headers={"Accept": "application/json"})
        with urllib.request.urlopen(req, timeout=min(30.0, self.timeout_sec)) as resp:
            out = resp.read().decode("utf-8")
            if not out.strip():
                return {}
            return json.loads(out)

    def health(self, *, online: bool = False) -> dict[str, Any]:
        base = self.online_base_url if online else self.build_base_url
        return self._get_json(base, "/health")

    def publish_index(
        self,
        table: str,
        *,
        input: str | list[str] | None = None,
        data_dir: str | None = None,
        schema_json: str | None = None,
        recreate: bool = False,
        build_id: str | None = None,
        cleanup_build_db_after_push: bool = False,
    ) -> dict[str, Any]:
        body: dict[str, Any] = {"table": table, "recreate": recreate}
        if schema_json is not None:
            body["schema_json"] = schema_json
        if build_id is not None:
            body["build_id"] = build_id
        if cleanup_build_db_after_push:
            body["cleanup_build_db_after_push"] = True
        if data_dir is not None:
            body["data_dir"] = data_dir
        if input is not None:
            body["input"] = input
        return self._post_json(self.build_base_url, "/publishIndex", body)

    def deploy_index(
        self,
        table: str,
        *,
        build_id: str | None = None,
        force_refresh: bool = False,
        max_local_versions: int = 2,
    ) -> dict[str, Any]:
        body: dict[str, Any] = {
            "table": table,
            "force_refresh": force_refresh,
            "max_local_versions": max_local_versions,
        }
        if build_id is not None:
            body["build_id"] = build_id
        return self._post_json(self.online_base_url, "/deployIndex", body)

    def switch_reload_index(self, table: str, *, build_id: str | None = None) -> dict[str, Any]:
        body: dict[str, Any] = {"table": table}
        if build_id is not None:
            body["build_id"] = build_id
        return self._post_json(self.online_base_url, "/switchReloadIndex", body)


def _default_urls() -> tuple[str, str]:
    b = os.environ.get("BUILD_AGENT_URL", "http://127.0.0.1:8787").strip()
    o = os.environ.get("ONLINE_AGENT_URL", b).strip()
    return b, o


def main() -> None:
    """Minimal CLI: publish then deploy using env BUILD_AGENT_URL / ONLINE_AGENT_URL."""
    import argparse

    ap = argparse.ArgumentParser(description="YikvPipelineClient demo (publish + deploy)")
    ap.add_argument("--table", required=True)
    ap.add_argument("--input", type=str, default=None, help="file, dir, cloud URI, or omit with --data-dir")
    ap.add_argument("--data-dir", type=str, default=None, dest="data_dir")
    ap.add_argument("--schema-json", type=str, default=None, dest="schema_json")
    ap.add_argument("--publish-only", action="store_true")
    ap.add_argument("--deploy-only", action="store_true")
    ap.add_argument("--deploy-build-id", type=str, default=None)
    ap.add_argument("--cleanup-after-push", action="store_true")
    args = ap.parse_args()

    build_u, online_u = _default_urls()
    c = YikvPipelineClient(build_u, online_u)

    if args.deploy_only:
        r = c.deploy_index(args.table, build_id=args.deploy_build_id)
        print(json.dumps(r, indent=2, ensure_ascii=False))
        return

    has_in = args.input is not None and args.input.strip() != ""
    has_dd = args.data_dir is not None and args.data_dir.strip() != ""
    if has_in == has_dd:
        raise SystemExit("provide exactly one of --input or --data-dir (unless --deploy-only)")

    pr = c.publish_index(
        args.table,
        input=args.input if has_in else None,
        data_dir=args.data_dir if has_dd else None,
        schema_json=args.schema_json,
        cleanup_build_db_after_push=args.cleanup_after_push,
    )
    print(json.dumps(pr, indent=2, ensure_ascii=False))
    if args.publish_only:
        return
    bid = args.deploy_build_id or pr.get("build_id")
    dr = c.deploy_index(args.table, build_id=bid if isinstance(bid, str) else None)
    print(json.dumps(dr, indent=2, ensure_ascii=False))


if __name__ == "__main__":
    main()
