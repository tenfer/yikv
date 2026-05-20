"""Build/push/pull/deploy/reload logic shared by FastAPI pipeline_agent and K8s Job entrypoints."""

from __future__ import annotations

import json
import os
import shutil
import socket
import subprocess
import sys
import threading
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Callable

from pipeline_config import PipelineSettings, load_pipeline_settings
from pipeline_models import (
    BuildIndexBody,
    DeployIndexBody,
    PipelineError,
    PublishIndexBody,
    PullIndexBody,
    PushIndexBody,
    SwitchReloadBody,
    is_cloud_uri,
    resolve_import_cli,
)

__all__ = [
    "PipelineContext",
    "PipelineError",
    "build_index",
    "push_index",
    "publish_index",
    "deploy_index",
    "pull_index",
    "switch_reload_index",
]


@dataclass(frozen=True)
class PipelineContext:
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
    import_bin: Path
    server_bin: Path
    schema_json: Path
    import_listen: str
    server_listen: str
    auto_start_server: bool
    arena_seg_gb: int
    arena_max_gb: int
    import_io_workers: int
    import_queue_batches: int

    @property
    def artifact_py(self) -> Path:
        return self.yikv_root / "tools" / "artifact_sync" / "artifact_sync.py"

    @classmethod
    def from_settings(cls, s: PipelineSettings) -> PipelineContext:
        return cls(
            yikv_root=s.yikv_root,
            work=s.work,
            build_db=s.build_db,
            server_db=s.server_db,
            artifact_store=s.artifact_store,
            artifact_key_prefix=s.artifact_key_prefix,
            artifact_env=s.artifact_env,
            artifact_config=s.artifact_config,
            admin_socket=s.admin_socket,
            server_config=s.server_config,
            import_bin=s.import_bin,
            server_bin=s.server_bin,
            schema_json=s.schema_json,
            import_listen=s.import_listen,
            server_listen=s.server_listen,
            auto_start_server=s.auto_start_server,
            arena_seg_gb=s.arena_seg_gb,
            arena_max_gb=s.arena_max_gb,
            import_io_workers=s.import_io_workers,
            import_queue_batches=s.import_queue_batches,
        )

    @classmethod
    def load(cls, base_dir: Path | None = None) -> PipelineContext:
        return cls.from_settings(load_pipeline_settings(base_dir))


def release_root(ctx: PipelineContext, table: str) -> Path:
    return (ctx.work / "releases" / table).resolve()


def ensure_artifact_config_local(ctx: PipelineContext) -> None:
    if ctx.artifact_config.is_file():
        return
    if ctx.artifact_config.resolve() == ctx.server_config.resolve():
        return
    ctx.work.mkdir(parents=True, exist_ok=True)
    ctx.artifact_store.mkdir(parents=True, exist_ok=True)
    body = {
        "artifact_storage": {
            "provider": "local",
            "env": ctx.artifact_env,
            "key_prefix": ctx.artifact_key_prefix,
            "local": {"root": str(ctx.artifact_store)},
        }
    }
    ctx.artifact_config.parent.mkdir(parents=True, exist_ok=True)
    ctx.artifact_config.write_text(json.dumps(body, indent=2) + "\n", encoding="utf-8")


def _write_import_config(ctx: PipelineContext, table: str) -> Path:
    agent_dir = ctx.work / ".pipeline_agent"
    agent_dir.mkdir(parents=True, exist_ok=True)
    cfg_path = agent_dir / f"import_{table}.json"
    body: dict[str, Any] = {
        "db_path": str(ctx.build_db),
        "listen": ctx.import_listen,
        "arena_seg_gb": ctx.arena_seg_gb,
        "arena_max_gb": ctx.arena_max_gb,
        "exclusive_arena_lock": False,
        "admin_unix_socket": str(ctx.admin_socket),
    }
    cfg_path.write_text(json.dumps(body, indent=2) + "\n", encoding="utf-8")
    return cfg_path


def _pipeline_progress_heartbeat_sec() -> float:
    """Wall-clock interval for 'still running' logs while a child process is alive. 0 disables."""
    raw = os.environ.get("YIKV_PIPELINE_PROGRESS_HEARTBEAT_SEC", "60").strip()
    if raw in ("", "0", "off", "false", "no"):
        return 0.0
    try:
        return max(0.0, float(raw))
    except ValueError:
        return 60.0


def _shallow_tree_bytes(path: Path, *, max_entries: int = 5000) -> int | None:
    """Cheap size hint for heartbeat (two directory levels, capped file walks)."""
    if not path.is_dir():
        return None
    total = 0
    n = 0
    try:
        for ent in path.iterdir():
            if n >= max_entries:
                return total
            try:
                if ent.is_file():
                    total += ent.stat().st_size
                    n += 1
                elif ent.is_dir():
                    for ent2 in ent.iterdir():
                        if n >= max_entries:
                            return total
                        if ent2.is_file():
                            total += ent2.stat().st_size
                            n += 1
            except OSError:
                continue
    except OSError:
        return None
    return total


def _run_subprocess_live(
    cmd: list[str],
    *,
    cwd: str,
    heartbeat_label: str,
    heartbeat_sec: float | None = None,
    heartbeat_extra: Callable[[], str] | None = None,
    env: dict[str, str] | None = None,
) -> subprocess.CompletedProcess[str]:
    """Run a subprocess with stdout/stderr streamed to this process (e.g. kubectl logs) and optional heartbeats.

    Unlike subprocess.run(capture_output=True), child logs appear immediately. Output is still collected
    for error reporting and build_log payloads.

    Env:
        YIKV_PIPELINE_PROGRESS_HEARTBEAT_SEC — seconds between heartbeats (0 disables).
    """
    if heartbeat_sec is None:
        heartbeat_sec = _pipeline_progress_heartbeat_sec()

    popen_kw: dict[str, Any] = {
        "cwd": cwd,
        "stdout": subprocess.PIPE,
        "stderr": subprocess.PIPE,
        "text": True,
        "bufsize": 1,
    }
    if env is not None:
        popen_kw["env"] = env
    proc = subprocess.Popen(cmd, **popen_kw)
    out_chunks: list[str] = []
    err_chunks: list[str] = []
    lock = threading.Lock()
    last_output = [time.monotonic()]

    def touch() -> None:
        with lock:
            last_output[0] = time.monotonic()

    def pump(pipe: Any, chunks: list[str], dest: Any) -> None:
        try:
            assert pipe is not None
            while True:
                line = pipe.readline()
                if line == "":
                    break
                chunks.append(line)
                dest.write(line)
                dest.flush()
                touch()
        finally:
            try:
                pipe.close()
            except OSError:
                pass

    t_out = threading.Thread(
        target=pump,
        args=(proc.stdout, out_chunks, sys.stdout),
        daemon=True,
    )
    t_err = threading.Thread(
        target=pump,
        args=(proc.stderr, err_chunks, sys.stderr),
        daemon=True,
    )
    t_out.start()
    t_err.start()

    stop_hb = threading.Event()
    t0 = time.monotonic()

    def heartbeat_loop() -> None:
        while not stop_hb.wait(timeout=heartbeat_sec):
            if proc.poll() is not None:
                return
            with lock:
                idle = time.monotonic() - last_output[0]
            elapsed = time.monotonic() - t0
            extra = ""
            if heartbeat_extra is not None:
                try:
                    s = heartbeat_extra().strip()
                    if s:
                        extra = f" {s}"
                except OSError:
                    pass
            print(
                f"{heartbeat_label}: progress heartbeat elapsed={elapsed:.0f}s "
                f"since_last_output={idle:.0f}s pid={proc.pid}{extra}",
                file=sys.stderr,
                flush=True,
            )

    hb_thread: threading.Thread | None = None
    if heartbeat_sec > 0:
        hb_thread = threading.Thread(target=heartbeat_loop, daemon=True)
        hb_thread.start()

    rc = proc.wait()
    stop_hb.set()
    if hb_thread is not None:
        hb_thread.join(timeout=2.0)
    t_out.join(timeout=120.0)
    t_err.join(timeout=120.0)

    stdout = "".join(out_chunks)
    stderr = "".join(err_chunks)
    return subprocess.CompletedProcess(cmd, rc, stdout, stderr)


def _run_artifact_sync(ctx: PipelineContext, args: list[str], *, need_config: bool) -> subprocess.CompletedProcess[str]:
    cmd = [os.environ.get("PYTHON", "python3"), str(ctx.artifact_py)]
    if need_config:
        cmd.extend(["-c", str(ctx.artifact_config)])
    cmd.extend(args)
    verb = args[0] if args else "artifact_sync"
    return _run_subprocess_live(
        cmd,
        cwd=str(ctx.yikv_root),
        heartbeat_label=f"artifact_sync:{verb}",
    )


def _ensure_executable(bin_path: Path, hint: str) -> None:
    if not bin_path.is_file():
        raise PipelineError(500, f"missing binary {bin_path} ({hint})")
    if not os.access(bin_path, os.X_OK):
        raise PipelineError(500, f"not executable: {bin_path} ({hint})")


def _admin_wait_max_sec() -> float:
    raw = os.environ.get("ADMIN_SOCKET_WAIT_SEC", "").strip()
    if raw:
        return max(5.0, float(raw))
    return 90.0


def _tail_file(path: Path, *, max_lines: int = 120, max_chars: int = 32000) -> str:
    if not path.is_file():
        return "(no log file yet)"
    try:
        data = path.read_bytes()
    except OSError as exc:
        return f"(cannot read log: {exc})"
    if len(data) > max_chars:
        data = data[-max_chars:]
    text = data.decode("utf-8", errors="replace")
    lines = text.splitlines()
    if len(lines) > max_lines:
        lines = lines[-max_lines:]
    return "\n".join(lines)


def _default_admin_unix_for_db_path(db_path_str: str) -> Path:
    dbp = Path(str(db_path_str).strip()).expanduser()
    return (dbp.parent / "admin.sock").resolve()


def _require_server_config_admin_socket(ctx: PipelineContext) -> None:
    try:
        cfg = json.loads(ctx.server_config.read_text(encoding="utf-8"))
    except FileNotFoundError:
        raise PipelineError(503, f"missing server config {ctx.server_config}") from None
    except json.JSONDecodeError as exc:
        raise PipelineError(503, f"invalid JSON in {ctx.server_config}: {exc}") from exc
    db_path = cfg.get("db_path")
    if not db_path or not str(db_path).strip():
        raise PipelineError(503, f"{ctx.server_config} missing db_path")
    raw = cfg.get("admin_unix_socket")
    if raw is None or not str(raw).strip():
        effective = _default_admin_unix_for_db_path(str(db_path))
    else:
        effective = Path(str(raw).strip()).expanduser().resolve()
    want = ctx.admin_socket.expanduser().resolve()
    if effective != want:
        raise PipelineError(
            503,
            (
                f"admin_unix_socket effective path {effective} != agent ADMIN_SOCKET {want}. "
                f"Set admin_unix_socket in {ctx.server_config} or align WORK/SERVER_DB/ADMIN_SOCKET "
                f"(default rule: parent(db_path)/admin.sock)."
            ),
        )


def _admin_unix_reachable(ctx: PipelineContext, *, timeout_sec: float = 2.0) -> bool:
    if not ctx.admin_socket.is_socket():
        return False
    try:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        try:
            s.settimeout(timeout_sec)
            s.connect(str(ctx.admin_socket))
        finally:
            s.close()
        return True
    except OSError:
        return False


def _wait_admin_unix_ready(
    ctx: PipelineContext,
    *,
    proc: subprocess.Popen[Any] | None,
    max_wait_sec: float,
    interval_sec: float = 0.2,
) -> None:
    log = ctx.work / "yikv_server.log"
    deadline = time.monotonic() + max_wait_sec
    while time.monotonic() < deadline:
        if proc is not None:
            rc = proc.poll()
            if rc is not None:
                tail = _tail_file(log)
                raise PipelineError(
                    500,
                    (
                        f"yikv_server exited before admin socket was ready (exit code {rc}). "
                        f"Binary {ctx.server_bin}. Config {ctx.server_config}.\n"
                        f"--- tail of {log} ---\n{tail}"
                    ),
                )
        if _admin_unix_reachable(ctx, timeout_sec=0.5):
            return
        time.sleep(interval_sec)
    tail = _tail_file(log)
    raise PipelineError(
        504,
        (
            f"yikv_server did not accept connections on {ctx.admin_socket} within {max_wait_sec:.0f}s "
            f"(ADMIN_SOCKET_WAIT_SEC). "
            f"Confirm {ctx.server_config} has admin_unix_socket matching this path and that "
            f"{ctx.server_bin} is rebuilt (admin listens before ScanAndLoad).\n"
            f"--- tail of {log} ---\n{tail}"
        ),
    )


def ensure_yikv_server_for_reload(ctx: PipelineContext) -> None:
    if _admin_unix_reachable(ctx):
        return
    if ctx.admin_socket.is_socket():
        try:
            ctx.admin_socket.unlink()
        except OSError:
            pass
    if not ctx.auto_start_server:
        raise PipelineError(
            503,
            f"admin socket not available: {ctx.admin_socket} (set AUTO_START_SERVER=1 to auto-start)",
        )
    if not ctx.server_config.is_file():
        raise PipelineError(
            503,
            f"missing server config {ctx.server_config}; create it manually (see config.example.json)",
        )
    _require_server_config_admin_socket(ctx)
    _ensure_executable(ctx.server_bin, "bazel build //:yikv_server")
    ctx.work.mkdir(parents=True, exist_ok=True)
    log = ctx.work / "yikv_server.log"
    pidfile = ctx.work / "yikv_server.pid"
    with open(log, "ab", buffering=0) as lf:
        p = subprocess.Popen(
            [str(ctx.server_bin), str(ctx.server_config)],
            cwd=str(ctx.yikv_root),
            stdout=lf,
            stderr=subprocess.STDOUT,
        )
    pidfile.write_text(str(p.pid), encoding="utf-8")
    _wait_admin_unix_ready(ctx, proc=p, max_wait_sec=_admin_wait_max_sec(), interval_sec=0.2)


def send_reload(ctx: PipelineContext, table: str) -> str:
    ensure_yikv_server_for_reload(ctx)

    def _exchange() -> str:
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        try:
            sock.settimeout(30.0)
            sock.connect(str(ctx.admin_socket))
            sock.sendall(f"reload {table}\n".encode())
            data = sock.recv(4096)
        finally:
            sock.close()
        return data.decode(errors="replace")

    try:
        reply = _exchange()
    except ConnectionRefusedError:
        if ctx.admin_socket.is_socket():
            try:
                ctx.admin_socket.unlink()
            except OSError:
                pass
        ensure_yikv_server_for_reload(ctx)
        reply = _exchange()
    rstrip = reply.strip()
    if not rstrip.startswith("ok"):
        raise PipelineError(
            500,
            (
                f"admin reload {table!r} on {ctx.admin_socket} did not succeed: {rstrip!r}. "
                f"Confirm the yikv_server process uses the same admin_unix_socket as this agent "
                f"(see SERVER_CONFIG / config parent(db_path)/admin.sock)."
            ),
        )
    return reply


def link_server_table(ctx: PipelineContext, table: str) -> None:
    if not ctx.server_config.is_file():
        ex = ctx.yikv_root / "config.example.json"
        raise PipelineError(
            503,
            (
                f"SERVER_CONFIG missing: {ctx.server_config}. Create it manually (e.g. copy {ex}); "
                f"db_path must be {ctx.server_db.resolve()} for this pipeline layout."
            ),
        )
    try:
        cfg = json.loads(ctx.server_config.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        raise PipelineError(503, f"invalid JSON in {ctx.server_config}: {exc}") from exc
    raw_db = cfg.get("db_path")
    if not raw_db or not str(raw_db).strip():
        raise PipelineError(503, f"{ctx.server_config} must set db_path to {ctx.server_db.resolve()}")
    cfg_db = Path(str(raw_db).strip()).expanduser().resolve()
    if cfg_db != ctx.server_db.resolve():
        raise PipelineError(
            503,
            (
                f"{ctx.server_config} db_path is {cfg_db}, expected {ctx.server_db.resolve()} "
                f"(SERVER_DB env / pipeline layout)"
            ),
        )
    rroot = release_root(ctx, table)
    ctx.server_db.mkdir(parents=True, exist_ok=True)
    tlink = ctx.server_db / table
    if tlink.exists() or tlink.is_symlink():
        tlink.unlink()
    tlink.symlink_to(rroot / "active", target_is_directory=True)


def _aws_env_for_cloud_import(ctx: PipelineContext) -> dict[str, str]:
    """Merge AWS SDK env vars from artifact_storage.s3_compatible for yikv_import_pipeline (Arrow S3)."""
    env = dict(os.environ)
    cfg = json.loads(ctx.server_config.read_text(encoding="utf-8"))
    art = cfg.get("artifact_storage") or {}
    if art.get("provider") != "s3_compatible":
        return env
    sc = art.get("s3_compatible") or {}
    ak = sc.get("access_key_id")
    sk = sc.get("secret_access_key")
    if ak:
        env["AWS_ACCESS_KEY_ID"] = str(ak)
    if sk:
        env["AWS_SECRET_ACCESS_KEY"] = str(sk)
    endpoint = sc.get("endpoint")
    if endpoint:
        env["AWS_ENDPOINT_URL_S3"] = str(endpoint)
    region = sc.get("region")
    if region:
        env["AWS_REGION"] = str(region)
    return env


def _validate_cloud_inputs_for_import(body: BuildIndexBody, ctx: PipelineContext) -> BuildIndexBody:
    """Validate cloud URIs; pass them through to yikv_import_pipeline (no rclone staging)."""
    _mode, paths = resolve_import_cli(body)
    cloud = [p for p in paths if is_cloud_uri(p)]
    if not cloud:
        return body
    local_paths = [p for p in paths if not is_cloud_uri(p)]
    if local_paths:
        raise PipelineError(
            400,
            "mixed cloud and local inputs are not supported; use only cloud URIs or only local paths",
        )
    cfg = json.loads(ctx.server_config.read_text(encoding="utf-8"))
    art = cfg.get("artifact_storage") or {}
    if art.get("provider") != "s3_compatible":
        raise PipelineError(
            400,
            "cloud input requires artifact_storage.provider s3_compatible in SERVER_CONFIG",
        )
    bucket_cfg = str((art.get("s3_compatible") or {}).get("bucket") or "")
    for uri in cloud:
        if not uri.strip().lower().startswith("s3://"):
            continue
        rest = uri.strip()[len("s3://") :]
        slash = rest.find("/")
        if slash < 0:
            raise PipelineError(400, f"invalid s3 URI: {uri}")
        bkt = rest[:slash]
        if bucket_cfg and bkt != bucket_cfg:
            raise PipelineError(
                400,
                f"s3 URI bucket {bkt!r} must match artifact_storage bucket {bucket_cfg!r}",
            )
    return body


def _remove_local_build_index(path: Path) -> None:
    if path.is_symlink() or path.is_file():
        path.unlink()
        return
    if path.is_dir():
        shutil.rmtree(path)


def build_index(ctx: PipelineContext, body: BuildIndexBody) -> dict[str, Any]:
    body = _validate_cloud_inputs_for_import(body, ctx)
    raw = body.schema_json or str(ctx.schema_json)
    schema = Path(raw)
    if not schema.is_absolute():
        schema = (ctx.yikv_root / schema).resolve()
    else:
        schema = schema.resolve()
    if not schema.is_file():
        raise PipelineError(400, f"schema_json not found: {schema}")
    _ensure_executable(ctx.import_bin, "bazel build //:yikv_import_pipeline")
    ctx.work.mkdir(parents=True, exist_ok=True)
    ctx.build_db.mkdir(parents=True, exist_ok=True)
    index_dir = ctx.build_db / body.table
    _remove_local_build_index(index_dir)
    cfg_path = _write_import_config(ctx, body.table)
    cmd: list[str] = [
        str(ctx.import_bin),
        "--config",
        str(cfg_path),
        "--index",
        body.table,
        "--schema_json",
        str(schema),
    ]
    extra: list[str] = ["--create_if_missing"]
    mode, paths = resolve_import_cli(body)
    if mode == "input_dir":
        cmd.extend(["--input_dir", paths[0]])
    else:
        for p in paths:
            cmd.extend(["--input", p])
    cmd.extend(extra)
    cmd.extend(
        [
            "--import_io_workers",
            str(ctx.import_io_workers),
            "--import_queue_batches",
            str(ctx.import_queue_batches),
        ]
    )

    def _import_heartbeat_extra() -> str:
        b = _shallow_tree_bytes(index_dir)
        if b is None:
            return ""
        return f"index_build_est_bytes={b}"

    _mode, _paths = resolve_import_cli(body)
    import_env = _aws_env_for_cloud_import(ctx) if any(is_cloud_uri(p) for p in _paths) else None
    r = _run_subprocess_live(
        cmd,
        cwd=str(ctx.yikv_root),
        heartbeat_label="yikv_import_pipeline",
        heartbeat_extra=_import_heartbeat_extra,
        env=import_env,
    )
    if r.returncode != 0:
        msg = (r.stderr or "") + (r.stdout or "")
        tail = msg.strip() or "yikv_import_pipeline failed"
        cmd_line = " ".join(cmd)
        raise PipelineError(500, f"import cmd (argv): {cmd_line}\n---\n{tail}")
    return {
        "ok": True,
        "table": body.table,
        "db_dir": str(ctx.build_db / body.table),
        "log": (r.stderr + r.stdout).strip() or None,
    }


def push_index(ctx: PipelineContext, body: PushIndexBody) -> dict[str, Any]:
    ensure_artifact_config_local(ctx)
    src = (ctx.build_db / body.table).resolve()
    if not src.is_dir():
        raise PipelineError(400, f"index directory missing (run buildIndex first): {src}")
    args = ["push", "--table", body.table, "--source", str(src), "--emit-build-id"]
    if body.build_id:
        args.extend(["--build-id", body.build_id])
    r = _run_artifact_sync(ctx, args, need_config=True)
    if r.returncode != 0:
        raise PipelineError(500, (r.stderr + r.stdout).strip() or "push failed")
    build_id = (r.stdout or "").strip().splitlines()[-1].strip() if r.stdout else ""
    if not build_id:
        raise PipelineError(500, "push produced no build_id on stdout")
    return {"ok": True, "build_id": build_id}


def _pull_stdout_build_id(proc: subprocess.CompletedProcess[str]) -> str:
    build_id = ""
    for line in (proc.stdout or "").splitlines():
        line = line.strip()
        if line:
            build_id = line
    return build_id


def publish_index(
    ctx: PipelineContext,
    body: PublishIndexBody,
    *,
    import_lock: threading.Lock | None = None,
) -> dict[str, Any]:
    def _run() -> dict[str, Any]:
        build_out = build_index(ctx, body)
        push_out = push_index(ctx, PushIndexBody(table=body.table, build_id=body.build_id))
        cleanup = body.cleanup_build_db_after_push or (
            os.environ.get("CLEANUP_BUILD_DB_AFTER_PUSH", "").strip().lower() in ("1", "true", "yes")
        )
        if cleanup:
            _remove_local_build_index(ctx.build_db / body.table)
        return {
            "ok": True,
            "build_id": push_out["build_id"],
            "table": body.table,
            "db_dir": build_out.get("db_dir"),
            "build_log": build_out.get("log"),
        }

    if import_lock is not None:
        with import_lock:
            return _run()
    return _run()


def deploy_index(ctx: PipelineContext, body: DeployIndexBody) -> dict[str, Any]:
    ensure_artifact_config_local(ctx)
    dest = release_root(ctx, body.table)
    dest.mkdir(parents=True, exist_ok=True)
    args: list[str] = [
        "pull",
        "--table",
        body.table,
        "--dest",
        str(dest),
        "--max-local-versions",
        str(body.max_local_versions),
        "--switch-active",
    ]
    if body.build_id:
        args.extend(["--build-id", body.build_id])
    if body.force_refresh:
        args.append("--force-refresh")
    r = _run_artifact_sync(ctx, args, need_config=True)
    if r.returncode != 0:
        raise PipelineError(500, (r.stderr + r.stdout).strip() or "pull failed")
    build_id = _pull_stdout_build_id(r)
    if not build_id:
        raise PipelineError(500, "pull produced no build_id")
    link_server_table(ctx, body.table)
    reload_reply = send_reload(ctx, body.table)
    return {
        "ok": True,
        "build_id": build_id,
        "dest": str(dest),
        "server_config": str(ctx.server_config),
        "reload_reply": reload_reply.strip() or None,
        "pull_stderr": r.stderr.strip() or None,
    }


def pull_index(ctx: PipelineContext, body: PullIndexBody) -> dict[str, Any]:
    ensure_artifact_config_local(ctx)
    dest = release_root(ctx, body.table)
    dest.mkdir(parents=True, exist_ok=True)
    args: list[str] = [
        "pull",
        "--table",
        body.table,
        "--dest",
        str(dest),
        "--max-local-versions",
        str(body.max_local_versions),
    ]
    if body.build_id:
        args.extend(["--build-id", body.build_id])
    if body.force_refresh:
        args.append("--force-refresh")
    if body.switch_active:
        args.append("--switch-active")
    r = _run_artifact_sync(ctx, args, need_config=True)
    if r.returncode != 0:
        raise PipelineError(500, (r.stderr + r.stdout).strip() or "pull failed")
    build_id = _pull_stdout_build_id(r)
    if not build_id:
        raise PipelineError(500, "pull produced no build_id")
    return {
        "ok": True,
        "build_id": build_id,
        "dest": str(dest),
        "stderr": r.stderr.strip() or None,
    }


def switch_reload_index(ctx: PipelineContext, body: SwitchReloadBody) -> dict[str, Any]:
    dest = release_root(ctx, body.table)
    if not dest.is_dir():
        raise PipelineError(400, f"release dest missing (run pullIndex first): {dest}")
    args: list[str] = ["switch", "--dest", str(dest)]
    if body.build_id:
        args.extend(["--build-id", body.build_id])
    r = _run_artifact_sync(ctx, args, need_config=False)
    if r.returncode != 0:
        raise PipelineError(500, (r.stderr + r.stdout).strip() or "switch failed")
    build_id_lines = [ln.strip() for ln in (r.stdout or "").splitlines() if ln.strip()]
    build_id = build_id_lines[-1] if build_id_lines else ""
    link_server_table(ctx, body.table)
    reload_reply = send_reload(ctx, body.table)
    return {
        "ok": True,
        "build_id": build_id or None,
        "server_config": str(ctx.server_config),
        "reload_reply": reload_reply.strip() or None,
    }
