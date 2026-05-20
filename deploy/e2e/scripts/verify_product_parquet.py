#!/usr/bin/env python3
"""Validate product Parquet vs deploy/e2e/fixtures/product/schema.json.

Always writes Markdown under repository root ``reports/`` (``--report`` / ``VERIFY_PRODUCT_REPORT``).

Run with ``~/.venv/bin/python`` (override check: ``VERIFY_SKIP_VENV_CHECK=1``).
"""

from __future__ import annotations

import argparse
import json
import operator
import os
import platform
import shlex
import re
import sys
import time
import traceback
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

REPORT_MD_VERSION = 1
CATEGORY_RE = re.compile(r"^([1-9][0-9]*)(,[1-9][0-9]*)*$")

_TYPE_CHECKERS: dict[str, Any] = {}


def _integral(v: Any) -> int:
    return operator.index(v)


def _script_dir() -> Path:
    return Path(__file__).resolve().parent


def _repo_root() -> Path:
    return Path(__file__).resolve().parents[3]


def _default_schema_path() -> Path:
    return (_script_dir() / "../fixtures/product/schema.json").resolve()


def _default_reports_dir() -> Path:
    return _repo_root() / "reports"


def _expected_venv_python() -> Path:
    return Path.home() / ".venv" / "bin" / "python"


def _using_expected_venv() -> bool:
    exp = _expected_venv_python()
    cur = Path(sys.executable)
    try:
        return exp.is_file() and cur.resolve() == exp.resolve()
    except OSError:
        return False


def _lazy_pyarrow_types() -> None:
    global _TYPE_CHECKERS
    if _TYPE_CHECKERS:
        return
    import pyarrow.types as pt

    _TYPE_CHECKERS["int64"] = pt.is_int64
    _TYPE_CHECKERS["int32"] = pt.is_int32
    _TYPE_CHECKERS["string"] = lambda t: pt.is_string(t) or pt.is_large_string(t)


def _atomic_write_text(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix(path.suffix + ".tmp")
    tmp.write_text(text, encoding="utf-8")
    tmp.replace(path)


def _arrow_type_matches(expected: str, field_type: Any) -> bool:
    _lazy_pyarrow_types()
    fn = _TYPE_CHECKERS.get(expected)
    return bool(fn and fn(field_type))


def _load_schema(schema_path: Path) -> tuple[list[tuple[str, str]], str]:
    raw = json.loads(schema_path.read_text(encoding="utf-8"))
    cols_raw = raw.get("columns") or raw.get("fields") or []
    cols: list[tuple[str, str]] = []
    for i, entry in enumerate(cols_raw):
        if not isinstance(entry, dict):
            raise ValueError(f"schema entry [{i}] must be object")
        name = str(entry.get("name", "")).strip()
        if not name:
            raise ValueError(f"schema entry [{i}].name required")
        t = (
            entry.get("arrow_type")
            or entry.get("type")
            or entry.get("parquet_type")
            or entry.get("data_type")
            or ""
        )
        if not isinstance(t, str) or not t.strip():
            raise ValueError(
                f"schema entry [{i}]: need arrow_type, type, parquet_type, or data_type"
            )
        cols.append((name, t.strip()))
    if not cols:
        raise ValueError("columns/fields is empty")
    pk_raw = raw.get("pk")
    ordered = [c[0] for c in cols]
    if pk_raw is None or str(pk_raw).strip() == "":
        return cols, ordered[0]
    return cols, str(pk_raw).strip()


def _md_escape_fence(text: str) -> str:
    return text.replace("```", "`\u200b``")


def _build_env_block() -> str:
    lines = [
        f"- **Python 可执行文件**: `{sys.executable}`",
        f"- **期望 venv** (`~/.venv/bin/python`): `{_expected_venv_python()}`",
        f"- **是否使用 ~/.venv**: {'是' if _using_expected_venv() else '否'}",
        f"- **Python**: `{sys.version.splitlines()[0]}`",
        f"- **platform**: `{platform.platform()}`",
    ]
    ve = os.environ.get("VIRTUAL_ENV", "").strip()
    if ve:
        lines.append(f"- **VIRTUAL_ENV**: `{ve}`")
    try:
        import pyarrow as pa

        lines.append(f"- **pyarrow**: `{pa.__version__}`")
    except ImportError:
        lines.append("- **pyarrow**: *未安装*")
    return "\n".join(lines)


def _verify(table: Any, cols: list[tuple[str, str]], pk_name: str) -> None:
    import pyarrow.compute as pc

    if table.num_columns != len(cols):
        raise ValueError(f"column count mismatch: parquet {table.num_columns} schema {len(cols)}")
    for i, (name, expect_type) in enumerate(cols):
        f = table.field(i)
        if f.name != name:
            raise ValueError(f"column {i} name: got {f.name!r} want {name!r}")
        if not _arrow_type_matches(expect_type, f.type):
            raise ValueError(f"column {name}: type {f.type} differs from schema {expect_type}")

    nrow = table.num_rows
    if nrow == 0:
        return

    required = {"id", "brand_id", "click_num", "title", "category_ids"}
    ordered = [c[0] for c in cols]
    if pk_name not in ordered:
        raise ValueError(f"pk {pk_name!r} missing from columns")
    if not required.issubset(set(ordered)):
        raise ValueError(f"missing required logical columns {sorted(required - set(ordered))}")

    id_chunk = table.column(pk_name).combine_chunks()
    if int(pc.count_distinct(id_chunk).as_py()) != nrow:
        raise ValueError(f"{pk_name} not unique (rows={nrow})")

    brand = table.column("brand_id").combine_chunks()
    for j in range(nrow):
        b = brand[j].as_py()
        if _integral(b) <= 0:
            raise ValueError(f"row {j} brand_id must be > 0, got {b!r}")

    clicks = table.column("click_num").combine_chunks()
    for j in range(nrow):
        cval = clicks[j].as_py()
        if _integral(cval) < 0:
            raise ValueError(f"row {j} click_num must be >= 0, got {cval!r}")

    titles = table.column("title").combine_chunks()
    for j in range(nrow):
        tv = titles[j].as_py()
        if tv is None:
            raise ValueError(f"row {j} title is null")
        s = tv if isinstance(tv, str) else str(tv)
        sl = len(s)
        if sl < 2 or sl > 50:
            raise ValueError(f"row {j} title unicode length must be [2,50], got {sl}")

    cats = table.column("category_ids").combine_chunks()
    for j in range(nrow):
        cv = cats[j].as_py()
        if cv is None:
            raise ValueError(f"row {j} category_ids is null")
        s = cv if isinstance(cv, str) else str(cv)
        if not CATEGORY_RE.fullmatch(s.strip()):
            raise ValueError(f"row {j} category_ids invalid: {s!r}")


def _tier0_operations_markdown(
    *,
    parquet_file: str,
    schema_file: str,
    skip_venv_check: bool,
    exit_code: int,
    rows_verified: int | None,
) -> str:
    """Human-readable list of what Tier-0 does (闭环中的离线数据门禁，非冒烟)."""
    venv_line = (
        "已跳过（环境变量 `VERIFY_SKIP_VENV_CHECK` 开启）。"
        if skip_venv_check
        else "要求当前进程即 `~/.venv/bin/python`，与仓库 `requirements.txt` 一致。"
    )
    rows_note = (
        f"本次 Parquet 行数 **{rows_verified}**，已执行行级规则。"
        if rows_verified is not None and rows_verified > 0
        else (
            "本次 Parquet 行数为 **0**，跳过行级字段规则。"
            if rows_verified == 0
            else "未读到有效行数（在本次失败点之前终止）。"
        )
    )
    outcome = {
        0: "在本脚本内**全部步骤已完成**，Tier-0 通过。",
        1: "在**行级或结构校验**处失败（见下方错误）。",
        2: "在**解释器 / IO / 未预期异常**处终止（见下方错误）。",
        3: "因 **pyarrow 未安装** 无法读 Parquet。",
        4: "在 **schema 或路径** 处失败。",
        5: "**报告文件无法写入**（磁盘权限或路径问题）。",
    }.get(exit_code, "见错误节。")

    return f"""### 本脚本在闭环中的角色

这是 **闭环测试的 Tier-0（离线门禁）**：在数据进入 `publishIndex` / 集群 Job 之前，确认 **Parquet 与契约一致**。与“冒烟”不同，此处校验 **完整契约与行级业务规则**（见下）。

### 按顺序执行的操作

1. **Python 环境**：{venv_line}
2. **读取契约文件**：`schema.json`（`{schema_file}`），解析列名、期望 Arrow 类型、主键列。
3. **加载 Parquet**：使用 `pyarrow.parquet.read_table` 读取 `{parquet_file}`。
4. **表结构校验**：列数量、列顺序、列名、各列 **PyArrow 类型** 与契约逐项一致。
5. **行数与行级规则**：{rows_note}  
   - 主键列唯一；`brand_id > 0`；`click_num ≥ 0`；`title` 非空且 Unicode 长度 2–50；`category_ids` 符合约定正则（正整数与逗号分隔）。
6. **写报告**：生成本 Markdown（耗时、环境、日志、错误）。

### 本轮结果摘要

- **退出码 `exit_code={exit_code}`**：{outcome}
"""


def _write_markdown_report(
    path: Path,
    *,
    argv_repr: str,
    started_utc: str,
    finished_utc: str,
    duration_s: float,
    exit_code: int,
    parquet_file: str,
    schema_file: str,
    rows_verified: int | None,
    console_lines: list[str],
    error_title: str | None,
    error_body: str | None,
    operations_md: str,
) -> None:
    status = "通过" if exit_code == 0 else "失败"
    log_content = _md_escape_fence("\n".join(console_lines) if console_lines else "（无）")
    err_block = ""
    if error_body:
        et = error_title or "错误"
        body = _md_escape_fence(error_body)
        err_block = f"\n## 错误与堆栈\n\n**{et}**\n\n```text\n{body}\n```\n"

    md = f"""# Product Parquet 验证报告 (Tier-0)

## 摘要

| 项 | 值 |
|----|----|
| 报告版本 | {REPORT_MD_VERSION} |
| 开始 (UTC) | {started_utc} |
| 结束 (UTC) | {finished_utc} |
| **耗时** | **{duration_s:.3f} s** |
| **退出码** | **{exit_code}** |
| **结论** | **{status}** |
| 校验行数 | {rows_verified if rows_verified is not None else "—"} |

## 执行的操作（Tier-0 闭环离线门禁）

{operations_md}

## 运行环境

{_build_env_block()}

## 命令行

```text
{argv_repr}
```

## 输入

- **Parquet**: `{parquet_file}`
- **schema.json**: `{schema_file}`

## 运行日志

```text
{log_content}
```
{err_block}
---
*由 `verify_product_parquet.py` 生成*
"""
    _atomic_write_text(path, md)


def main() -> int:
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    default_schema = _default_schema_path()
    reports_dir = _default_reports_dir()

    parser = argparse.ArgumentParser(description="Validate product Parquet (Tier-0); writes Markdown report.")
    parser.add_argument("parquet_file", type=Path)
    parser.add_argument("--schema", type=Path, default=None, help=f"default: {default_schema}")
    parser.add_argument(
        "--report",
        type=Path,
        default=None,
        help="Markdown report path (default: reports/product-parquet-verify-<UTC>.md)",
    )
    args = parser.parse_args()
    skip_venv_check = os.environ.get("VERIFY_SKIP_VENV_CHECK", "").strip() in ("1", "true", "yes")

    argv_repr = " ".join(shlex.quote(a) for a in sys.argv)

    parquet_file = args.parquet_file.expanduser().resolve()
    schema_path = (args.schema or default_schema).expanduser().resolve()
    reports_dir.mkdir(parents=True, exist_ok=True)

    if args.report is not None:
        report_path = args.report.expanduser().resolve()
    elif os.environ.get("VERIFY_PRODUCT_REPORT", "").strip():
        report_path = Path(os.environ["VERIFY_PRODUCT_REPORT"].strip()).expanduser().resolve()
    else:
        report_path = reports_dir / f"product-parquet-verify-{stamp}.md"

    t0 = time.perf_counter()
    started_utc = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")

    console: list[str] = []

    def log(msg: str) -> None:
        console.append(msg)
        print(msg, flush=True)

    exit_code = 2
    err_title: str | None = None
    err_body: str | None = None
    rows_verified: int | None = None
    schema_loaded = False

    def finish(code: int) -> int:
        nonlocal exit_code
        exit_code = code
        duration = time.perf_counter() - t0
        finished = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
        console.append(f"verify_product_parquet: report_written={report_path}")
        try:
            ops = _tier0_operations_markdown(
                parquet_file=str(parquet_file),
                schema_file=str(schema_path),
                skip_venv_check=skip_venv_check,
                exit_code=exit_code,
                rows_verified=rows_verified,
            )
            _write_markdown_report(
                report_path,
                argv_repr=f"{shlex.quote(sys.executable)} {argv_repr}",
                started_utc=started_utc,
                finished_utc=finished,
                duration_s=duration,
                exit_code=exit_code,
                parquet_file=str(parquet_file),
                schema_file=str(schema_path),
                rows_verified=rows_verified,
                console_lines=console,
                error_title=err_title,
                error_body=err_body,
                operations_md=ops,
            )
        except OSError as exc:
            sys.stderr.write(f"verify_product_parquet: cannot write report {report_path}: {exc}\n")
            return 5
        print(console[-1], flush=True)
        return exit_code

    if not skip_venv_check and not _using_expected_venv():
        err_title = "Python 解释器不符合要求"
        err_body = (
            f"请使用 ~/.venv/bin/python 运行（当前: {sys.executable}）。\n"
            f"期望: {_expected_venv_python()}\n"
            "或设置 VERIFY_SKIP_VENV_CHECK=1（不推荐）。"
        )
        log(err_body)
        return finish(2)

    try:
        if not schema_path.is_file():
            err_title = "schema 文件不存在"
            err_body = str(schema_path)
            log(f"ERROR {err_body}")
            return finish(4)

        try:
            import pyarrow.parquet as pq  # noqa: PLC0415
        except ImportError as exc:
            err_title = "缺少 pyarrow"
            err_body = "pip install -r requirements.txt（需在 ~/.venv 中安装）\n" + str(exc)
            log(err_body)
            return finish(3)

        try:
            cols, pk_name = _load_schema(schema_path)
            schema_loaded = True
            log(f"schema primary key: {pk_name!r}, columns: {[c[0] for c in cols]}")
        except (json.JSONDecodeError, ValueError) as exc:
            err_title = "schema.json 无效"
            err_body = str(exc)
            log(f"ERROR {err_body}")
            return finish(4)

        if not parquet_file.is_file():
            err_title = "Parquet 文件不存在"
            err_body = str(parquet_file)
            log(f"ERROR {err_body}")
            return finish(2)

        try:
            tbl = pq.read_table(parquet_file)
        except OSError as exc:
            err_title = "读取 Parquet 失败"
            err_body = str(exc)
            log(f"ERROR {err_body}")
            return finish(2)

        nrow = tbl.num_rows
        rows_verified = nrow
        log(f"parquet rows={nrow}")

        _verify(tbl, cols, pk_name)
        log("OK all row/column checks passed")
        return finish(0)

    except ValueError as exc:
        err_title = "校验失败（契约/数据）" if schema_loaded else "schema 错误"
        err_body = str(exc)
        log(f"ERROR {err_body}")
        return finish(1 if schema_loaded else 4)
    except Exception as exc:
        err_title = type(exc).__name__
        err_body = "".join(traceback.format_exception(exc))
        log(err_body)
        return finish(2)


if __name__ == "__main__":
    raise SystemExit(main())
