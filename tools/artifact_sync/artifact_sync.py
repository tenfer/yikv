#!/usr/bin/env python3
"""Config-driven artifact publish/sync: local directory or S3-compatible storage (OSS/OBS/COS/S3/MinIO).

Reads JSON with top-level key "artifact_storage" (same file shape as optional yikv_server config extension).
No central «current» pointer in the store:
«latest» = lexicographically greatest build_id under the table prefix (use YYYYMMDDHHmmss from push).

pull: fetches exactly one version (--build-id or default latest). Optional --max-local-versions keeps
only the newest N dirs under --dest. switch: local symlink dest/active -> build_id (or --latest).
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
from datetime import datetime
from pathlib import Path
from typing import Any

RCLONE_REMOTE = "yikv_artifact"
_BUILD_ID_AUTO = frozenset({None, "", "auto"})


def load_config(path: Path) -> dict[str, Any]:
    """Load artifact root from JSON: top-level ``artifact_storage`` object (provider, env, key_prefix, …)."""
    raw = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(raw, dict):
        raise SystemExit(f"JSON config must be an object: {path}")
    if "artifact_storage" not in raw:
        raise SystemExit(f'JSON config {path} must contain top-level "artifact_storage"')
    data = raw["artifact_storage"]
    if not isinstance(data, dict):
        raise SystemExit(f"artifact_storage must be an object in {path}")
    return data


def _resolve_secret(cfg: dict[str, Any], key: str, env_key_name: str | None) -> str:
    if key in cfg and cfg[key]:
        return str(cfg[key])
    envn = cfg.get(env_key_name) if env_key_name else None
    if envn:
        v = os.environ.get(str(envn))
        if v:
            return v
    return ""


def table_dir_rel(env: str, key_prefix: str, table: str) -> str:
    return "/".join([env.strip("/"), key_prefix.strip("/"), table.strip("/")])


def build_prefix_rel(env: str, key_prefix: str, table: str, build_id: str) -> str:
    return table_dir_rel(env, key_prefix, table) + "/" + build_id.strip("/")


def ensure_rclone(cfg: dict[str, Any]) -> Path:
    """Write temporary rclone.ini; return path."""
    sc = cfg.get("s3_compatible") or {}
    access_key = _resolve_secret(sc, "access_key_id", "access_key_id_env")
    secret_key = _resolve_secret(sc, "secret_access_key", "secret_access_key_env")
    if not access_key or not secret_key:
        raise SystemExit("s3_compatible: set access_key_id/secret_access_key or *_env and export those variables")
    bucket = sc.get("bucket") or ""
    endpoint = sc.get("endpoint") or ""
    region = sc.get("region") or ""
    if not bucket or not endpoint:
        raise SystemExit("s3_compatible: bucket and endpoint are required")

    force_path_style = sc.get("force_path_style", False)
    ini_lines = [
        f"[{RCLONE_REMOTE}]",
        "type = s3",
        "provider = Other",
        "env_auth = false",
        f"access_key_id = {access_key}",
        f"secret_access_key = {secret_key}",
        f"endpoint = {endpoint}",
        f"region = {region}",
    ]
    if force_path_style:
        ini_lines.append("force_path_style = true")
    fd, name = tempfile.mkstemp(prefix="rclone-yikv-", suffix=".ini")
    os.close(fd)
    p = Path(name)
    p.write_text("\n".join(ini_lines) + "\n", encoding="utf-8")
    return p


def run_rclone(rcfg: Path | None, args: list[str]) -> None:
    cmd = ["rclone"]
    if rcfg is not None:
        cmd.extend(["--config", str(rcfg)])
    cmd.extend(args)
    r = subprocess.run(cmd, check=False)
    if r.returncode != 0:
        raise SystemExit(f"rclone failed ({r.returncode}): {' '.join(cmd)}")


def _rclone_lsf_dirs(rcfg: Path, bucket: str, prefix_slash: str) -> list[str]:
    """prefix_slash: e.g. dev/yikv-index/mytable/ — list immediate child dir names."""
    remote = f"{RCLONE_REMOTE}:{bucket}/{prefix_slash}"
    r = subprocess.run(
        ["rclone", "--config", str(rcfg), "lsf", remote, "--dirs-only"],
        capture_output=True,
        text=True,
        check=False,
    )
    if r.returncode != 0:
        return []
    out: list[str] = []
    for line in r.stdout.splitlines():
        name = line.strip().rstrip("/")
        if name:
            out.append(name)
    return out


def list_build_ids_in_store(cfg: dict[str, Any], table: str) -> list[str]:
    env = str(cfg.get("env", "dev"))
    key_prefix = str(cfg.get("key_prefix", "yikv-index"))
    rel = table_dir_rel(env, key_prefix, table)
    provider = cfg.get("provider")
    if provider == "local":
        root = Path(cfg["local"]["root"]).expanduser().resolve()
        d = root / rel
        if not d.is_dir():
            return []
        return [p.name for p in d.iterdir() if p.is_dir()]
    if provider == "s3_compatible":
        rcfg = ensure_rclone(cfg)
        try:
            bucket = cfg["s3_compatible"]["bucket"]
            return _rclone_lsf_dirs(rcfg, bucket, rel + "/")
        finally:
            rcfg.unlink(missing_ok=True)
    raise SystemExit(f"Unknown provider: {provider}")


def resolve_build_id_for_pull(cfg: dict[str, Any], table: str, explicit: str | None) -> str:
    if explicit:
        return explicit.strip("/")
    ids = list_build_ids_in_store(cfg, table)
    if not ids:
        raise SystemExit(f"no build directories under table {table!r} in artifact store")
    return max(ids)


def _log(msg: str, *, emit_build_id_only: bool) -> None:
    if not emit_build_id_only:
        print(msg, file=sys.stderr)


def _build_id_timestamp_local() -> str:
    return datetime.now().strftime("%Y%m%d%H%M%S")


def _allocate_build_id(cfg: dict[str, Any], env: str, key_prefix: str, table: str, emit_build_id_only: bool) -> str:
    provider = cfg.get("provider")
    root: Path | None = None
    if provider == "local":
        root = Path(cfg["local"]["root"]).expanduser().resolve()

    suffix = 0
    while True:
        base = _build_id_timestamp_local()
        build_id = base if suffix == 0 else f"{base}-{suffix}"
        if provider == "local" and root is not None:
            dest = root / build_prefix_rel(env, key_prefix, table, build_id)
            if not dest.exists():
                _log(f"allocated build_id={build_id}", emit_build_id_only=emit_build_id_only)
                return build_id
            suffix += 1
            if suffix > 99:
                time.sleep(1)
                suffix = 0
            continue
        _log(f"allocated build_id={build_id}", emit_build_id_only=emit_build_id_only)
        return build_id


def _resolve_push_build_id(args: argparse.Namespace, cfg: dict[str, Any]) -> str:
    raw = args.build_id
    if raw in _BUILD_ID_AUTO:
        return _allocate_build_id(
            cfg, str(cfg.get("env", "dev")), str(cfg.get("key_prefix", "yikv-index")), args.table, args.emit_build_id
        )
    return str(raw).strip("/")


def cmd_push(args: argparse.Namespace, cfg: dict[str, Any]) -> None:
    provider = cfg.get("provider")
    env = str(cfg.get("env", "dev"))
    key_prefix = str(cfg.get("key_prefix", "yikv-index"))
    table = args.table
    build_id = _resolve_push_build_id(args, cfg)
    src = Path(args.source).resolve()
    if not src.is_dir():
        raise SystemExit(f"source is not a directory: {src}")

    rel = build_prefix_rel(env, key_prefix, table, build_id)

    if provider == "local":
        root = Path(cfg["local"]["root"]).expanduser().resolve()
        dest = root / rel
        dest.parent.mkdir(parents=True, exist_ok=True)
        if dest.exists():
            shutil.rmtree(dest)
        shutil.copytree(src, dest)
        _log(f"local: copied to {dest}", emit_build_id_only=args.emit_build_id)
    elif provider == "s3_compatible":
        rcfg = ensure_rclone(cfg)
        try:
            bucket = cfg["s3_compatible"]["bucket"]
            remote_path = f"{RCLONE_REMOTE}:{bucket}/{rel}"
            run_rclone(rcfg, ["copy", str(src), remote_path, "--s3-no-check-bucket"])
            _log(f"s3: copied to {bucket}/{rel}", emit_build_id_only=args.emit_build_id)
        finally:
            rcfg.unlink(missing_ok=True)
    else:
        raise SystemExit(f"Unknown provider: {provider}")

    print(build_id)


def _pull_one_local(
    root: Path, env: str, key_prefix: str, table: str, build_id: str, dest_root: Path, force: bool
) -> None:
    src = root / build_prefix_rel(env, key_prefix, table, build_id)
    if not src.is_dir():
        raise SystemExit(f"build directory missing in store: {src}")
    out = dest_root / build_id
    out.parent.mkdir(parents=True, exist_ok=True)
    if out.exists() and not force:
        print(f"pull: skip existing {out}", file=sys.stderr)
        return
    if out.exists():
        shutil.rmtree(out)
    shutil.copytree(src, out)
    print(f"pull: local {src} -> {out}", file=sys.stderr)


def _pull_one_s3(
    rcfg: Path,
    bucket: str,
    env: str,
    key_prefix: str,
    table: str,
    build_id: str,
    dest_root: Path,
    force: bool,
) -> None:
    rel = build_prefix_rel(env, key_prefix, table, build_id)
    remote_path = f"{RCLONE_REMOTE}:{bucket}/{rel}"
    out = dest_root / build_id
    out.parent.mkdir(parents=True, exist_ok=True)
    if out.exists() and not force:
        print(f"pull: skip existing {out}", file=sys.stderr)
        return
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)
    run_rclone(rcfg, ["copy", remote_path, str(out)])
    print(f"pull: s3 {bucket}/{rel} -> {out}", file=sys.stderr)


def _prune_local_versions(dest_root: Path, keep_n: int, must_keep: str) -> None:
    """Keep at most keep_n version dirs; must_keep is never removed (evict oldest among kept if over cap)."""
    if keep_n <= 0:
        return
    dest_root.mkdir(parents=True, exist_ok=True)
    version_dirs = [
        p.name
        for p in dest_root.iterdir()
        if p.is_dir() and p.name not in ("active",) and not p.name.startswith(".")
    ]
    if not version_dirs:
        return
    combined = set(version_dirs) | {must_keep}
    ordered = sorted(combined, reverse=True)
    keep: set[str] = set()
    for x in ordered:
        if len(keep) >= keep_n:
            break
        keep.add(x)
    if must_keep not in keep:
        keep.add(must_keep)
    while len(keep) > keep_n:
        keep.remove(min(keep))
    for name in version_dirs:
        if name not in keep:
            victim = dest_root / name
            print(f"pull: prune old version dir {victim}", file=sys.stderr)
            shutil.rmtree(victim)


def _atomic_symlink(dest_link: Path, target_rel: str) -> None:
    dest_link.parent.mkdir(parents=True, exist_ok=True)
    tmp = dest_link.with_name(dest_link.name + ".tmp")
    if tmp.exists() or tmp.is_symlink():
        tmp.unlink()
    tmp.symlink_to(target_rel, target_is_directory=True)
    os.replace(tmp, dest_link)


def cmd_pull(args: argparse.Namespace, cfg: dict[str, Any]) -> None:
    provider = cfg.get("provider")
    env = str(cfg.get("env", "dev"))
    key_prefix = str(cfg.get("key_prefix", "yikv-index"))
    table = args.table
    dest_root = Path(args.dest).expanduser().resolve()

    build_id = resolve_build_id_for_pull(cfg, table, args.build_id)
    print(f"pull: using build_id={build_id}", file=sys.stderr)

    if provider == "local":
        root = Path(cfg["local"]["root"]).expanduser().resolve()
        _pull_one_local(root, env, key_prefix, table, build_id, dest_root, args.force_refresh)
    elif provider == "s3_compatible":
        rcfg = ensure_rclone(cfg)
        try:
            bucket = cfg["s3_compatible"]["bucket"]
            _pull_one_s3(rcfg, bucket, env, key_prefix, table, build_id, dest_root, args.force_refresh)
        finally:
            rcfg.unlink(missing_ok=True)
    else:
        raise SystemExit(f"Unknown provider: {provider}")

    _prune_local_versions(dest_root, args.max_local_versions, build_id)

    if args.switch_active:
        link = dest_root / "active"
        _atomic_symlink(link, build_id)
        print(f"pull: active -> {build_id}", file=sys.stderr)

    print(build_id)


def _local_version_names(dest_root: Path) -> list[str]:
    names = []
    for p in dest_root.iterdir():
        if p.is_dir() and p.name not in ("active",) and not p.name.startswith("."):
            names.append(p.name)
    return names


def resolve_build_id_for_switch(dest_root: Path, explicit: str | None) -> str:
    if explicit:
        bid = explicit.strip("/")
        if not (dest_root / bid).is_dir():
            raise SystemExit(f"no local directory {dest_root / bid!s}")
        return bid
    names = _local_version_names(dest_root)
    if not names:
        raise SystemExit(f"no version subdirs under {dest_root}")
    return max(names)


def cmd_switch(args: argparse.Namespace, cfg: dict[str, Any] | None) -> None:
    _ = cfg
    dest_root = Path(args.dest).expanduser().resolve()
    build_id = resolve_build_id_for_switch(dest_root, args.build_id)
    link = dest_root / "active"
    _atomic_symlink(link, build_id)
    print(f"switch: {link} -> {build_id}", file=sys.stderr)
    print(build_id)


def cmd_versions(args: argparse.Namespace, cfg: dict[str, Any]) -> None:
    ids = sorted(list_build_ids_in_store(cfg, args.table))
    for i in ids:
        print(i)
    if not ids:
        print("(empty)", file=sys.stderr)


def cmd_print_rclone(args: argparse.Namespace, cfg: dict[str, Any]) -> None:
    if cfg.get("provider") != "s3_compatible":
        raise SystemExit("print-rclone-config only for provider: s3_compatible")
    rcfg = ensure_rclone(cfg)
    try:
        print(rcfg.read_text(encoding="utf-8"))
        print("# Temporary file:", rcfg, file=sys.stderr)
    finally:
        rcfg.unlink(missing_ok=True)


def main() -> None:
    ap = argparse.ArgumentParser(description="yikv artifact sync (local / S3-compatible)")
    ap.add_argument(
        "-c",
        "--config",
        type=Path,
        default=None,
        help='JSON file with top-level "artifact_storage" (push/pull/versions/print-rclone-config)',
    )

    sub = ap.add_subparsers(dest="cmd", required=True)

    p_push = sub.add_parser("push", help="upload a built index directory as one build_id")
    p_push.add_argument("--table", required=True)
    p_push.add_argument(
        "--build-id",
        default=None,
        help="omit or 'auto' for local time YYYYMMDDHHmmss",
    )
    p_push.add_argument("--source", required=True, help="local directory produced by yikv_import_pipeline")
    p_push.add_argument("--emit-build-id", action="store_true", help="stderr logs only; stdout is build id")
    p_push.set_defaults(func=cmd_push)

    p_pull = sub.add_parser(
        "pull",
        help="copy one version from store to --dest/<build_id>/ (default: latest by lexicographic max)",
    )
    p_pull.add_argument("--table", required=True)
    p_pull.add_argument(
        "--dest",
        required=True,
        help="directory for per-version subdirs and optional symlink 'active'",
    )
    p_pull.add_argument(
        "--build-id",
        default=None,
        help="explicit version; omit to use latest in store (max name among children)",
    )
    p_pull.add_argument(
        "--max-local-versions",
        type=int,
        default=2,
        help="after pull, keep only the newest N version subdirs under --dest (0=disable prune)",
    )
    p_pull.add_argument("--force-refresh", action="store_true", help="re-copy even if dest/<build_id> exists")
    p_pull.add_argument(
        "--switch-active",
        action="store_true",
        help="after pull, atomically symlink dest/active -> this build_id",
    )
    p_pull.set_defaults(func=cmd_pull)

    p_sw = sub.add_parser(
        "switch",
        help="local only: atomically symlink dest/active -> version subdir (no -c needed)",
    )
    p_sw.add_argument("--dest", required=True, help="directory that holds build_id/ subdirs (same as pull --dest)")
    p_sw.add_argument(
        "--build-id",
        default=None,
        help="subdir name to point to; omit to pick lexicographically largest existing subdir (typical «newest» timestamp)",
    )
    p_sw.set_defaults(func=cmd_switch)

    p_ver = sub.add_parser("versions", help="list build_id directories in the artifact store for a table")
    p_ver.add_argument("--table", required=True)
    p_ver.set_defaults(func=cmd_versions)

    p_pr = sub.add_parser("print-rclone-config", help="debug: print generated rclone s3 snippet")
    p_pr.set_defaults(func=cmd_print_rclone)

    args = ap.parse_args()

    need_cfg = args.cmd in ("push", "pull", "versions", "print-rclone-config")
    if need_cfg and args.config is None:
        ap.error(f"command {args.cmd!r} requires -c/--config")

    cfg: dict[str, Any] | None = load_config(args.config) if need_cfg else None
    args.func(args, cfg)


if __name__ == "__main__":
    main()
