#!/usr/bin/env python3
"""Apply embedded MinIO Deployment/Service/Secret and create the artifact bucket.

Requires: pip install -r requirements.txt (from repo root; kubernetes, minio, etc.).

Does not modify the plan file. Intended for dev/small clusters; rotate credentials for production.
"""

from __future__ import annotations

import argparse
import base64
import json
import os
import sys
import time
from pathlib import Path

import yaml
from kubernetes import client, config
from kubernetes.client import ApiException
from kubernetes.utils import create_from_yaml
from urllib3.exceptions import MaxRetryError


def _wait_deployment_ready(
    apps: client.AppsV1Api,
    namespace: str,
    name: str,
    *,
    timeout_sec: float = 300.0,
) -> None:
    deadline = time.monotonic() + timeout_sec
    while time.monotonic() < deadline:
        try:
            d = apps.read_namespaced_deployment_status(name, namespace)
            if d.status and d.status.ready_replicas and d.status.ready_replicas >= 1:
                return
        except ApiException:
            pass
        time.sleep(2.0)
    raise SystemExit(f"timeout waiting for Deployment/{name} in {namespace}")


class _KubeYamlLoader(yaml.loader.SafeLoader):
    """Match kubernetes.utils.create_from_yaml Loader (avoid '=' timestamp parsing issues)."""

    yaml_implicit_resolvers = yaml.loader.SafeLoader.yaml_implicit_resolvers.copy()
    if "=" in yaml_implicit_resolvers:
        yaml_implicit_resolvers.pop("=")


def _yaml_documents(manifest: str) -> list[dict]:
    docs: list[dict] = []
    for doc in yaml.load_all(manifest, Loader=_KubeYamlLoader):
        if doc is not None:
            if not isinstance(doc, dict):
                raise SystemExit("bootstrap_minio: manifest must be YAML mappings (dicts), not scalars/lists at top level")
            docs.append(doc)
    return docs


def main() -> int:
    p = argparse.ArgumentParser(description="Bootstrap MinIO in-cluster and create bucket.")
    p.add_argument("--namespace", default="yikv", help="Kubernetes namespace")
    p.add_argument("--bucket", default="yikv-artifacts", help="S3 bucket to create")
    p.add_argument(
        "--manifest",
        type=Path,
        default=Path(__file__).resolve().with_name("minio-embedded.yaml"),
        help="YAML manifest (namespace placeholder NAMESPACE_PLACEHOLDER)",
    )
    p.add_argument("--dry-run", action="store_true", help="Print actions only")
    p.add_argument(
        "--kubeconfig",
        type=Path,
        default=None,
        help="Path to kubeconfig (default: same resolution as kubectl — KUBECONFIG or ~/.kube/config)",
    )
    p.add_argument(
        "--minio-s3-host",
        default=None,
        metavar="HOST",
        help=(
            "MinIO S3 API host for this script only. On your laptop, cluster DNS "
            "(minio.<namespace>.svc.cluster.local) does not resolve — use 127.0.0.1 and run "
            "`kubectl port-forward -n NAMESPACE svc/minio 9000:9000` in another terminal."
        ),
    )
    p.add_argument(
        "--minio-s3-port",
        type=int,
        default=9000,
        metavar="PORT",
        help="MinIO S3 API port (used with --minio-s3-host; default 9000).",
    )
    p.add_argument(
        "--bucket-only",
        action="store_true",
        help="Do not kubectl-apply manifest; MinIO Deployment must already exist (create bucket + print JSON only).",
    )
    args = p.parse_args()

    raw = args.manifest.read_text(encoding="utf-8")
    rendered = raw.replace("NAMESPACE_PLACEHOLDER", args.namespace)

    if args.dry_run:
        print(rendered)
        return 0

    try:
        if args.kubeconfig is not None:
            config.load_kube_config(config_file=str(args.kubeconfig))
        else:
            config.load_kube_config()
    except config.ConfigException as kube_exc:
        # Avoid misleading "Service host/port is not set" when not running inside a pod.
        host = os.getenv("KUBERNETES_SERVICE_HOST", "").strip()
        port = os.getenv("KUBERNETES_SERVICE_PORT", "").strip()
        if host and port:
            try:
                config.load_incluster_config()
            except config.ConfigException as incluster_exc:
                print(
                    f"bootstrap_minio: kube config: {kube_exc}; in-cluster: {incluster_exc}",
                    file=sys.stderr,
                )
                return 2
        else:
            print(
                f"bootstrap_minio: kube config: {kube_exc}\n"
                "hint: ensure `kubectl cluster-info` works in this environment, "
                "or pass --kubeconfig /path/to/kubeconfig.",
                file=sys.stderr,
            )
            return 2

    core = client.CoreV1Api()
    apps = client.AppsV1Api()

    try:
        core.read_namespace(args.namespace)
    except ApiException as e:
        if e.status == 404:
            print(f"bootstrap_minio: namespace {args.namespace!r} does not exist; create it first", file=sys.stderr)
            return 3
        raise
    except MaxRetryError as e:
        print(
            "bootstrap_minio: cannot reach the Kubernetes API (connection failed). "
            "Your kubeconfig likely points at 127.0.0.1:PORT — that tunnel or proxy is not listening.\n"
            "  Check: kubectl cluster-info\n"
            "  If you use kubectl proxy / port-forward, start it again, then re-run this script.",
            file=sys.stderr,
        )
        print(f"  (underlying error: {e.reason})", file=sys.stderr)
        return 2

    k8s = client.ApiClient()
    if not args.bucket_only:
        create_from_yaml(
            k8s,
            yaml_objects=_yaml_documents(rendered),
            verbose=False,
            namespace=args.namespace,
        )

    _wait_deployment_ready(apps, args.namespace, "minio")

    sec = core.read_namespaced_secret("minio-root", args.namespace)
    if not sec.data:
        raise SystemExit("minio-root secret missing data")

    user = base64.b64decode(sec.data.get("root-user", b"")).decode("utf-8")
    password = base64.b64decode(sec.data.get("root-password", b"")).decode("utf-8")

    # In-cluster services must use cluster DNS. This script may run on a laptop; then override
    # reachability with --minio-s3-host (e.g. 127.0.0.1 + kubectl port-forward).
    cluster_endpoint_host = f"minio.{args.namespace}.svc.cluster.local"
    cluster_endpoint_url = f"http://{cluster_endpoint_host}:9000"
    s3_host = args.minio_s3_host or cluster_endpoint_host
    s3_port = args.minio_s3_port

    from minio import Minio  # type: ignore[import-untyped]

    mc = Minio(f"{s3_host}:{s3_port}", access_key=user, secret_key=password, secure=False)
    try:
        if not mc.bucket_exists(args.bucket):
            mc.make_bucket(args.bucket)
    except MaxRetryError as e:
        reason = str(e.reason) if e.reason is not None else str(e)
        if "Failed to resolve" in reason or "Name or service not known" in reason:
            print(
                "bootstrap_minio: cannot reach MinIO S3 API (DNS/network).\n"
                "  In-cluster hostname only resolves inside the cluster.\n"
                "  From this machine: in another terminal run\n"
                f"    kubectl port-forward -n {args.namespace} svc/minio 9000:9000\n"
                "  Then re-run with:\n"
                "    --minio-s3-host 127.0.0.1 --minio-s3-port 9000",
                file=sys.stderr,
            )
            print(f"  (detail: {reason})", file=sys.stderr)
            return 4
        raise

    artifact_storage = {
        "artifact_storage": {
            "provider": "s3_compatible",
            "env": "dev",
            "key_prefix": "yikv-index",
            "s3_compatible": {
                "bucket": args.bucket,
                "endpoint": cluster_endpoint_url,
                "region": "shanghai",
                "access_key_id": user,
                "secret_access_key": password,
                "force_path_style": True,
            },
        }
    }
    print("--- artifact_storage JSON (merge into SERVER_CONFIG / ARTIFACT_CONFIG) ---")
    print(json.dumps(artifact_storage, indent=2))
    print("--- Kubernetes Secret data key suggestions (do not commit real secrets) ---")
    print(f"  in-cluster endpoint (pods): {cluster_endpoint_url}")
    print(f"  bucket: {args.bucket}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
