#!/usr/bin/env python3
"""Single place for pipeline / agent path + arena + listen defaults.

Resolution order:
  1) Built-in defaults
  2) Merge tools/pipeline_agent/pipeline.defaults.json when present
  3) Merge optional JSON from $PIPELINE_CONFIG when set and exists
  4) Merge server JSON ($SERVER_CONFIG or $WORK/config.server.json, or pipeline `server_config`)
     when present: arena_*, listen -> server_listen, optional artifact_storage (env, key_prefix,
     local.root). Same file can be passed to yikv_server and to artifact_sync -c.
  5) Environment variables override (highest priority)

Long-running import / artifact_sync (pipeline_ops subprocesses): set
``YIKV_PIPELINE_PROGRESS_HEARTBEAT_SEC`` (seconds, default 60; ``0`` disables) for periodic
``… progress heartbeat …`` lines on stderr while children are alive.

CLI:
  python3 pipeline_config.py --print-json
  python3 pipeline_config.py --emit-shell [CONFIG.json]
"""

from __future__ import annotations

import argparse
import json
import os
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any


@dataclass
class PipelineSettings:
    yikv_root: Path
    work: Path
    build_db: Path
    server_db: Path
    artifact_store: Path
    artifact_key_prefix: str
    artifact_env: str
    artifact_config: Path
    admin_socket: Path
    server_config: Path
    import_listen: str
    server_listen: str
    arena_seg_gb: int
    arena_max_gb: int
    import_io_workers: int
    import_queue_batches: int
    import_bin: Path
    server_bin: Path
    schema_json: Path
    auto_start_server: bool


def agent_dir() -> Path:
    return Path(__file__).resolve().parent


def _deep_merge(base: dict[str, Any], overlay: dict[str, Any]) -> None:
    for k, v in overlay.items():
        if k in base and isinstance(base[k], dict) and isinstance(v, dict):
            _deep_merge(base[k], v)
        else:
            base[k] = v


def _load_json_object(path: Path) -> dict[str, Any]:
    data = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(data, dict):
        raise SystemExit(f"pipeline config must be a JSON object: {path}")
    return data


def _deep_merge_from_server_config(server_cfg_path: Path, merged: dict[str, Any]) -> None:
    """Overlay fields shared with yikv_server config.json and optional artifact_storage."""
    if not server_cfg_path.is_file():
        return
    try:
        sc = json.loads(server_cfg_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return
    if not isinstance(sc, dict):
        return

    had_artifact = False
    art = sc.get("artifact_storage")
    if isinstance(art, dict):
        had_artifact = True
        ev = art.get("env")
        if isinstance(ev, str) and ev.strip():
            merged["artifact_env"] = ev.strip()
        kp = art.get("key_prefix")
        if isinstance(kp, str) and kp.strip():
            merged["artifact_key_prefix"] = kp.strip()
        prov = str(art.get("provider") or "local").strip()
        loc = art.get("local") or {}
        if prov == "local" and isinstance(loc, dict):
            root = loc.get("root")
            if isinstance(root, str) and root.strip():
                merged["artifact_store"] = root.strip()

    if "arena_seg_gb" in sc:
        merged["arena_seg_gb"] = int(sc["arena_seg_gb"])
    if "arena_max_gb" in sc:
        merged["arena_max_gb"] = int(sc["arena_max_gb"])
    listen = sc.get("listen")
    if isinstance(listen, str) and listen.strip():
        merged["server_listen"] = listen.strip()

    if had_artifact and not os.environ.get("ARTIFACT_CONFIG"):
        ay = merged.get("artifact_config")
        if not ay or (isinstance(ay, str) and not ay.strip()):
            merged["artifact_config"] = str(server_cfg_path.resolve())


def load_pipeline_settings(base_dir: Path | None = None) -> PipelineSettings:
    ad = base_dir or agent_dir()
    yikv_root = Path(os.environ.get("YIKV_ROOT") or (ad.parent.parent)).resolve()

    merged: dict[str, Any] = {
        "work": "/data/yikvdb",
        "artifact_key_prefix": "yikv-index",
        "artifact_env": "dev",
        "import_listen": "127.0.0.1:59999",
        "server_listen": "0.0.0.0:9000",
        "arena_seg_gb": 1,
        "arena_max_gb": 4,
        "import_io_workers": 1,
        "import_queue_batches": 8,
        "auto_start_server": True,
    }

    defaults_path = ad / "pipeline.defaults.json"
    if defaults_path.is_file():
        _deep_merge(merged, _load_json_object(defaults_path))

    work = Path(os.environ.get("WORK") or merged["work"]).resolve()

    cfg_env = os.environ.get("PIPELINE_CONFIG", "").strip()
    if cfg_env:
        pc = Path(cfg_env).expanduser()
        if pc.is_file():
            _deep_merge(merged, _load_json_object(pc))

    def server_config_candidate() -> Path:
        raw = os.environ.get("SERVER_CONFIG", "").strip()
        if raw:
            return Path(raw).expanduser().resolve()
        sc = merged.get("server_config")
        if isinstance(sc, str) and sc.strip():
            return Path(sc.strip()).expanduser().resolve()
        return (work / "config.server.json").resolve()

    _deep_merge_from_server_config(server_config_candidate(), merged)

    def pick_path(env_name: str, json_key: str, relative_under_work: str) -> Path:
        if os.environ.get(env_name):
            return Path(os.environ[env_name]).expanduser().resolve()
        if json_key in merged and merged[json_key]:
            return Path(str(merged[json_key])).expanduser().resolve()
        return (work / relative_under_work).resolve()

    artifact_key_prefix = os.environ.get("ARTIFACT_KEY_PREFIX") or str(
        merged.get("artifact_key_prefix") or "yikv-index"
    )
    artifact_env = os.environ.get("ARTIFACT_ENV") or str(merged.get("artifact_env") or "dev")
    import_listen = os.environ.get("IMPORT_LISTEN") or str(merged["import_listen"])
    server_listen = os.environ.get("SERVER_LISTEN") or str(merged["server_listen"])

    arena_seg = int(os.environ.get("PIPELINE_ARENA_SEG_GB") or merged["arena_seg_gb"])
    arena_max = int(os.environ.get("PIPELINE_ARENA_MAX_GB") or merged["arena_max_gb"])

    def _import_int(env_key: str, json_key: str, fallback: int) -> int:
        ev = os.environ.get(env_key, "").strip()
        if ev:
            return max(1, int(ev))
        raw = merged.get(json_key)
        if raw is not None:
            return max(1, int(raw))
        return max(1, fallback)

    import_io_workers = _import_int("IMPORT_IO_WORKERS", "import_io_workers", 1)
    import_queue_batches = _import_int("IMPORT_QUEUE_BATCHES", "import_queue_batches", 8)

    ast = os.environ.get("AUTO_START_SERVER", str(merged["auto_start_server"]).lower())
    auto_start = ast == "1" or ast.lower() == "true"

    return PipelineSettings(
        yikv_root=yikv_root,
        work=work,
        build_db=pick_path("BUILD_DB", "build_db", "build_db"),
        server_db=pick_path("SERVER_DB", "server_db", "server_db"),
        artifact_store=pick_path("ARTIFACT_STORE", "artifact_store", "artifact_store"),
        artifact_key_prefix=artifact_key_prefix,
        artifact_env=artifact_env,
        artifact_config=pick_path("ARTIFACT_CONFIG", "artifact_config", "artifact-storage.json"),
        admin_socket=pick_path("ADMIN_SOCKET", "admin_socket", "admin.sock"),
        server_config=pick_path("SERVER_CONFIG", "server_config", "config.server.json"),
        import_listen=import_listen,
        server_listen=server_listen,
        arena_seg_gb=arena_seg,
        arena_max_gb=arena_max,
        import_io_workers=import_io_workers,
        import_queue_batches=import_queue_batches,
        import_bin=Path(
            os.environ.get("YIKV_IMPORT_BIN", str(yikv_root / "bazel-bin" / "yikv_import_pipeline"))
        ),
        server_bin=Path(
            os.environ.get("YIKV_SERVER_BIN", str(yikv_root / "bazel-bin" / "yikv_server"))
        ),
        schema_json=Path(
            os.environ.get("SCHEMA_JSON", str(yikv_root / "schema.json"))
        ).expanduser().resolve(),
        auto_start_server=auto_start,
    )


def emit_shell(s: PipelineSettings) -> str:
    import shlex

    def q(p: Path) -> str:
        return shlex.quote(str(p))

    lines = [
        f"export WORK={q(s.work)}",
        f"export BUILD_DB={q(s.build_db)}",
        f"export SERVER_DB={q(s.server_db)}",
        f"export ARTIFACT_STORE={q(s.artifact_store)}",
        f"export ARTIFACT_KEY_PREFIX={shlex.quote(s.artifact_key_prefix)}",
        f"export ARTIFACT_ENV={shlex.quote(s.artifact_env)}",
        f"export ARTIFACT_CONFIG={q(s.artifact_config)}",
        f"export ADMIN_SOCKET={q(s.admin_socket)}",
        f"export SERVER_CONFIG={q(s.server_config)}",
        f"export IMPORT_LISTEN={shlex.quote(s.import_listen)}",
        f"export SERVER_LISTEN={shlex.quote(s.server_listen)}",
        f"export PIPELINE_ARENA_SEG_GB={s.arena_seg_gb}",
        f"export PIPELINE_ARENA_MAX_GB={s.arena_max_gb}",
        f"export IMPORT_IO_WORKERS={s.import_io_workers}",
        f"export IMPORT_QUEUE_BATCHES={s.import_queue_batches}",
        f"export AUTO_START_SERVER={'1' if s.auto_start_server else '0'}",
    ]
    return "\n".join(lines) + "\n"


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--print-json", action="store_true")
    ap.add_argument("--emit-shell", action="store_true")
    ap.add_argument("config_path", nargs="?", help="optional JSON; also sets PIPELINE_CONFIG for this run")
    args = ap.parse_args()

    if args.config_path:
        os.environ["PIPELINE_CONFIG"] = args.config_path

    s = load_pipeline_settings()

    if args.emit_shell:
        sys.stdout.write(emit_shell(s))
        return

    if args.print_json:
        d = {
            "yikv_root": str(s.yikv_root),
            "work": str(s.work),
            "build_db": str(s.build_db),
            "server_db": str(s.server_db),
            "artifact_store": str(s.artifact_store),
            "artifact_key_prefix": s.artifact_key_prefix,
            "artifact_env": s.artifact_env,
            "artifact_config": str(s.artifact_config),
            "admin_socket": str(s.admin_socket),
            "server_config": str(s.server_config),
            "import_listen": s.import_listen,
            "server_listen": s.server_listen,
            "arena_seg_gb": s.arena_seg_gb,
            "arena_max_gb": s.arena_max_gb,
            "import_io_workers": s.import_io_workers,
            "import_queue_batches": s.import_queue_batches,
            "import_bin": str(s.import_bin),
            "server_bin": str(s.server_bin),
            "schema_json": str(s.schema_json),
            "auto_start_server": s.auto_start_server,
        }
        print(json.dumps(d, indent=2))
        return

    ap.print_help()


if __name__ == "__main__":
    main()
