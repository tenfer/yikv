#!/usr/bin/env python3
"""Create a Kubernetes Job that runs pipeline deploy_index in-cluster (shared PVC).

Uses the ``pipeline-worker`` image; it reads DEPLOY_SPEC_JSON (same JSON as POST /deployIndex).

Mounts persistentVolumeClaim ``yikv-data`` at /data so SERVER_DB=/data/db and admin_unix_socket
match the running yikv_server pod. Set AUTO_START_SERVER=0 (reload uses existing server).

Requires: pip install kubernetes (repo root requirements.txt).

Example:
  python3 submit_deploy_job.py --namespace yikv --image pipeline-worker:latest \\
    --spec-json '{"table":"product","build_id":"20260518120000"}' \\
    --configmap yikv-e2e-publish-config --pvc-claim yikv-data --wait
"""

from __future__ import annotations

import argparse
import json
import sys
import time
from pathlib import Path

from kubernetes import client, config
from kubernetes.client import ApiException


def _build_deploy_job(
    *,
    namespace: str,
    image: str,
    spec_json: str,
    extra_env: dict[str, str],
    generate_name: str,
    ttl_seconds: int,
    active_deadline: int,
    backoff_limit: int,
    service_account: str | None,
    configmap_name: str | None,
    configmap_mount_path: str,
    configmap_key: str,
    pvc_claim: str,
    pvc_mount_path: str,
) -> client.V1Job:
    env_vars = [client.V1EnvVar(name="DEPLOY_SPEC_JSON", value=spec_json)]
    merged_env = dict(extra_env)
    if configmap_name:
        cfg_file = f"{configmap_mount_path.rstrip('/')}/{configmap_key}"
        merged_env.setdefault("SERVER_CONFIG", cfg_file)
        merged_env.setdefault("ARTIFACT_CONFIG", cfg_file)
    # Align with server-config.json db_path=/data/db; releases under WORK.
    merged_env.setdefault("WORK", f"{pvc_mount_path.rstrip('/')}/yikvdb")
    merged_env.setdefault("SERVER_DB", f"{pvc_mount_path.rstrip('/')}/db")
    merged_env.setdefault("ADMIN_SOCKET", f"{pvc_mount_path.rstrip('/')}/admin.sock")
    merged_env.setdefault("AUTO_START_SERVER", "0")
    for k, v in sorted(merged_env.items()):
        env_vars.append(client.V1EnvVar(name=k, value=v))

    volume_mounts: list[client.V1VolumeMount] = [
        client.V1VolumeMount(name="yikv-data", mount_path=pvc_mount_path),
    ]
    volumes: list[client.V1Volume] = [
        client.V1Volume(
            name="yikv-data",
            persistent_volume_claim=client.V1PersistentVolumeClaimVolumeSource(
                claim_name=pvc_claim,
            ),
        ),
    ]
    if configmap_name:
        volume_mounts.append(
            client.V1VolumeMount(
                name="job-config",
                mount_path=configmap_mount_path,
                read_only=True,
            ),
        )
        volumes.append(
            client.V1Volume(
                name="job-config",
                config_map=client.V1ConfigMapVolumeSource(name=configmap_name),
            ),
        )

    pod_spec = client.V1PodSpec(
        restart_policy="Never",
        containers=[
            client.V1Container(
                name="deploy",
                image=image,
                image_pull_policy="IfNotPresent",
                command=["python3", "run_deploy_job.py"],
                env=env_vars,
                volume_mounts=volume_mounts,
            ),
        ],
        volumes=volumes,
    )
    if service_account:
        pod_spec.service_account_name = service_account
    return client.V1Job(
        api_version="batch/v1",
        kind="Job",
        metadata=client.V1ObjectMeta(generate_name=generate_name, namespace=namespace),
        spec=client.V1JobSpec(
            ttl_seconds_after_finished=ttl_seconds,
            backoff_limit=backoff_limit,
            active_deadline_seconds=active_deadline,
            template=client.V1PodTemplateSpec(
                metadata=client.V1ObjectMeta(
                    labels={"app.kubernetes.io/name": "yikv-pipeline-deploy"},
                ),
                spec=pod_spec,
            ),
        ),
    )


def _wait_job(batch: client.BatchV1Api, namespace: str, name: str, *, timeout_sec: float) -> int:
    deadline = time.monotonic() + timeout_sec
    while time.monotonic() < deadline:
        j = batch.read_namespaced_job_status(name, namespace)
        if j.status:
            succ = j.status.succeeded or 0
            failed = j.status.failed or 0
            if succ >= 1:
                return 0
            if failed >= 1:
                return 1
        time.sleep(2.0)
    print(f"submit_deploy_job: timeout waiting for Job/{name}", file=sys.stderr)
    return 2


def main() -> int:
    p = argparse.ArgumentParser(description="Submit deploy Job (deploy_index) via Kubernetes API.")
    p.add_argument("--namespace", default="yikv")
    p.add_argument("--image", default="pipeline-worker:latest")
    p.add_argument(
        "--spec-file",
        type=Path,
        default=None,
        help="JSON file validated as DeployIndexBody",
    )
    p.add_argument(
        "--spec-json",
        default="",
        help="Inline JSON (alternative to --spec-file)",
    )
    p.add_argument("--generate-name", default="yikv-deploy-", help="Job metadata.generateName")
    p.add_argument("--ttl-seconds-after-finished", type=int, default=86400)
    p.add_argument("--active-deadline-seconds", type=int, default=3600)
    p.add_argument("--backoff-limit", type=int, default=0)
    p.add_argument("--service-account", default="", help="Pod serviceAccountName (optional)")
    p.add_argument("--set-env", action="append", default=[], metavar="KEY=VAL", help="Extra env")
    p.add_argument(
        "--configmap",
        default="",
        metavar="NAME",
        help="Mount ConfigMap at --configmap-mount; sets SERVER_CONFIG / ARTIFACT_CONFIG",
    )
    p.add_argument("--configmap-mount", default="/etc/yikv-job")
    p.add_argument("--configmap-key", default="config.json")
    p.add_argument("--pvc-claim", default="yikv-data", help="PVC claim name (yikv server data)")
    p.add_argument("--pvc-mount-path", default="/data")
    p.add_argument("--wait", action="store_true")
    p.add_argument("--wait-timeout-sec", type=float, default=3600.0)
    args = p.parse_args()

    if args.spec_file:
        spec_text = args.spec_file.read_text(encoding="utf-8")
    elif args.spec_json.strip():
        spec_text = args.spec_json.strip()
    else:
        print("submit_deploy_job: need --spec-file or --spec-json", file=sys.stderr)
        return 2
    try:
        json.loads(spec_text)
    except json.JSONDecodeError as exc:
        print(f"submit_deploy_job: invalid JSON: {exc}", file=sys.stderr)
        return 2

    extra: dict[str, str] = {}
    for item in args.set_env:
        if "=" not in item:
            print(f"submit_deploy_job: bad --set-env {item!r}", file=sys.stderr)
            return 2
        k, v = item.split("=", 1)
        extra[k.strip()] = v

    try:
        config.load_kube_config()
    except config.ConfigException:
        try:
            config.load_incluster_config()
        except config.ConfigException as exc:
            print(f"submit_deploy_job: kube config: {exc}", file=sys.stderr)
            return 2

    batch = client.BatchV1Api()
    job = _build_deploy_job(
        namespace=args.namespace,
        image=args.image,
        spec_json=spec_text,
        extra_env=extra,
        generate_name=args.generate_name,
        ttl_seconds=args.ttl_seconds_after_finished,
        active_deadline=args.active_deadline_seconds,
        backoff_limit=args.backoff_limit,
        service_account=args.service_account or None,
        configmap_name=args.configmap or None,
        configmap_mount_path=args.configmap_mount,
        configmap_key=args.configmap_key,
        pvc_claim=args.pvc_claim,
        pvc_mount_path=args.pvc_mount_path,
    )
    try:
        created = batch.create_namespaced_job(args.namespace, job)
    except ApiException as exc:
        print(f"submit_deploy_job: create Job failed: {exc}", file=sys.stderr)
        return 3
    name = created.metadata.name if created.metadata else ""
    print(name)
    if args.wait and name:
        return _wait_job(batch, args.namespace, name, timeout_sec=args.wait_timeout_sec)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
