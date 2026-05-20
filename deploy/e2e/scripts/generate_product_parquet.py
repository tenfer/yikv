#!/usr/bin/env python3
"""Generate product Parquet rows matching deploy/e2e/fixtures/product/schema.json.

Schema uses engine-native ``fields`` + ``data_type`` (verify script also accepts legacy ``columns``).

Uses pyarrow + numpy. For ``--rows`` above ``--stream-below`` (default 500_000), writes via
``ParquetWriter`` in chunks to avoid OOM (suitable for 100M+ rows).

Env: ``PYTHONUNBUFFERED=1`` recommended for long runs logging progress.
"""

from __future__ import annotations

import argparse
import random
import re
import sys
import time
from pathlib import Path

import numpy as np
import pyarrow as pa
import pyarrow.compute as pc
import pyarrow.parquet as pq

CATEGORY_RE = re.compile(r"^([1-9][0-9]*)(,[1-9][0-9]*)*$")


def _title_at(i: int) -> str:
    s = f"pd{i % 50_000:05d}a{(i * 7) % 10000}"
    if len(s) < 8:
        s = s + "xy"
    return s[:50]


def _category_at(i: int) -> str:
    a = (i % 98) + 1
    b = ((i // 100) % 98) + 1
    if i % 3 == 0:
        return str(a)
    return f"{a},{b}"


def _table_schema() -> pa.Schema:
    return pa.schema(
        [
            ("id", pa.int64()),
            ("brand_id", pa.int32()),
            ("click_num", pa.int32()),
            ("title", pa.string()),
            ("category_ids", pa.string()),
        ]
    )


def _self_check_parquet(path: Path, n: int) -> None:
    t = pq.read_table(path)
    if t.num_rows != n:
        raise SystemExit(f"self-check: row count got {t.num_rows} want {n}")
    if pc.count_distinct(t.column("id")).as_py() != n:
        raise SystemExit("self-check: id not unique")
    if pc.min(t.column("brand_id")).as_py() < 1:
        raise SystemExit("self-check: brand_id min < 1")
    if pc.min(t.column("click_num")).as_py() < 0:
        raise SystemExit("self-check: click_num min < 0")
    rng = random.Random(0)
    idxs = list(range(n))
    for j in rng.sample(idxs, min(500, n)):
        tv = t.column("title")[j].as_py()
        sl = len(tv if isinstance(tv, str) else str(tv))
        if sl < 2 or sl > 50:
            raise SystemExit(f"self-check: title len row {j}: {sl}")
        cv = t.column("category_ids")[j].as_py()
        s = cv if isinstance(cv, str) else str(cv)
        if not CATEGORY_RE.fullmatch(s.strip()):
            raise SystemExit(f"self-check: category_ids row {j}: {s!r}")


def _self_check_metadata_only(path: Path, n: int) -> None:
    pf = pq.ParquetFile(path)
    got = pf.metadata.num_rows
    if got != n:
        raise SystemExit(f"self-check (metadata): row count got {got} want {n}")
    if pf.metadata.num_row_groups < 1:
        raise SystemExit("self-check (metadata): no row groups")


def _write_streaming(
    path: Path,
    n: int,
    *,
    chunk_rows: int,
    compression: str,
) -> None:
    schema = _table_schema()
    t0 = time.monotonic()
    with pq.ParquetWriter(path, schema, compression=compression) as writer:
        written = 0
        while written < n:
            end = min(written + chunk_rows, n)
            idx = np.arange(written, end, dtype=np.int64)
            ids = idx + 1
            brand = ((idx * 17) % 1000 + 1).astype(np.int32)
            clicks = ((idx * 13) % 1_000_000).astype(np.int32)
            ws = written
            titles = [_title_at(i) for i in range(ws, end)]
            cats = [_category_at(i) for i in range(ws, end)]
            batch = pa.table(
                {
                    "id": pa.array(ids, type=pa.int64()),
                    "brand_id": pa.array(brand, type=pa.int32()),
                    "click_num": pa.array(clicks, type=pa.int32()),
                    "title": pa.array(titles, type=pa.string()),
                    "category_ids": pa.array(cats, type=pa.string()),
                },
                schema=schema,
            )
            writer.write_table(batch)
            written = end
            dt = time.monotonic() - t0
            rate = written / dt if dt > 0 else 0.0
            print(
                f"  chunk -> {written}/{n} rows ({rate:,.0f} rows/s)",
                file=sys.stderr,
                flush=True,
            )


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--rows", type=int, default=100_000, help="Row count (default 100000)")
    ap.add_argument("-o", "--output", type=Path, required=True, help="Output .parquet path")
    ap.add_argument(
        "--chunk-rows",
        type=int,
        default=1_000_000,
        help="Row groups for streaming mode (default 1000000)",
    )
    ap.add_argument(
        "--stream-below",
        type=int,
        default=500_000,
        help="Use single-table write if rows <= this; else ParquetWriter streaming (default 500000)",
    )
    ap.add_argument(
        "--compression",
        default="snappy",
        help="Parquet compression codec (default snappy)",
    )
    ap.add_argument(
        "--self-check",
        action="store_true",
        help="After write: full scan (heavy for 100M+; prefer --self-check-lite)",
    )
    ap.add_argument(
        "--self-check-lite",
        action="store_true",
        help="After write: Parquet metadata row count only (default for rows > 1_000_000)",
    )
    args = ap.parse_args()
    n = args.rows
    if n < 1:
        raise SystemExit("--rows must be >= 1")
    if args.chunk_rows < 10_000:
        raise SystemExit("--chunk-rows must be >= 10000")

    args.output.parent.mkdir(parents=True, exist_ok=True)
    use_stream = n > args.stream_below

    if use_stream:
        print(
            f"streaming write: {n:,} rows, chunk={args.chunk_rows:,} -> {args.output}",
            file=sys.stderr,
            flush=True,
        )
        _write_streaming(
            args.output,
            n,
            chunk_rows=args.chunk_rows,
            compression=args.compression,
        )
    else:
        ids = list(range(1, n + 1))
        brand = [((i * 17) % 1000) + 1 for i in range(n)]
        clicks = [(i * 13) % 1_000_000 for i in range(n)]
        titles = [_title_at(i) for i in range(n)]
        cats = [_category_at(i) for i in range(n)]
        table = pa.table(
            {
                "id": pa.array(ids, type=pa.int64()),
                "brand_id": pa.array(brand, type=pa.int32()),
                "click_num": pa.array(clicks, type=pa.int32()),
                "title": pa.array(titles, type=pa.string()),
                "category_ids": pa.array(cats, type=pa.string()),
            }
        )
        pq.write_table(table, args.output, compression=args.compression)

    print(f"wrote {n} rows -> {args.output}")

    lite_default = n > 1_000_000
    if args.self_check:
        if n > 5_000_000:
            print(
                "warning: --self-check loads entire file; consider --self-check-lite for huge files",
                file=sys.stderr,
            )
        _self_check_parquet(args.output, n)
        print("self-check ok")
    elif args.self_check_lite or lite_default:
        _self_check_metadata_only(args.output, n)
        print("self-check-lite ok (metadata row count)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
