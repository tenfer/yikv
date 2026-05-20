#!/usr/bin/env python3
"""Upload one file to MinIO using the minio Python SDK (already in requirements.txt)."""
from __future__ import annotations

import argparse
import sys
from urllib.parse import urlparse

try:
    from minio import Minio
    from minio.error import S3Error
except ImportError:
    print("upload_parquet_minio: pip install minio", file=sys.stderr)
    raise SystemExit(2)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("local_path")
    ap.add_argument("--bucket", default="yikv-artifacts")
    ap.add_argument("--key", required=True)
    ap.add_argument(
        "--endpoint",
        default="http://127.0.0.1:30900",
        help="S3 API base URL (Kind hostPort default :30900; port-forward often :9000)",
    )
    ap.add_argument("--access-key", default="minio")
    ap.add_argument("--secret-key", default="minio12345")
    args = ap.parse_args()

    u = urlparse(args.endpoint)
    if not u.hostname:
        print("upload_parquet_minio: bad --endpoint", file=sys.stderr)
        return 2
    secure = u.scheme == "https"
    port = u.port if u.port is not None else (443 if secure else 9000)
    netloc = f"{u.hostname}:{port}"
    client = Minio(
        netloc,
        access_key=args.access_key,
        secret_key=args.secret_key,
        secure=secure,
        region="",
    )
    try:
        if not client.bucket_exists(args.bucket):
            client.make_bucket(args.bucket)
    except S3Error as exc:
        print(f"upload_parquet_minio: bucket: {exc}", file=sys.stderr)
        return 1
    try:
        client.fput_object(args.bucket, args.key, args.local_path)
    except S3Error as exc:
        print(f"upload_parquet_minio: put: {exc}", file=sys.stderr)
        return 1
    print(f"ok: s3://{args.bucket}/{args.key} via {args.endpoint}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
