#!/usr/bin/env python3
"""Create a Kubernetes Job that runs pipeline publish (pipeline_ops) in-cluster.

Uses the Kubernetes API only (no HTTP to spawn the Job). The Job container should use the
``pipeline-worker`` image; it reads PUBLISH_SPEC_JSON (same JSON as POST /publishIndex).

Requires: pip install -r requirements.txt (repo root).

Example:
  python3 submit_publish_job.py --namespace yikv --image pipeline-worker:latest \\
    --spec-file publish-spec.json --configmap yikv-e2e-publish-config --work-on-emptydir
"""

from __future__ import annotations

import argparse
import json
import sys
import time
from pathlib import Path

from kubernetes import client, config
from kubernetes.client import ApiException


def _build_job(
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
    work_emptydir_path: str | None,
) -> client.V1Job:
    env_vars = [client.V1EnvVar(name="PUBLISH_SPEC_JSON", value=spec_json)]
    merged_env = dict(extra_env)
    if configmap_name:
        cfg_file = f"{configmap_mount_path.rstrip('/')}/{configmap_key}"
        merged_env.setdefault("SERVER_CONFIG", cfg_file)
        merged_env.setdefault("ARTIFACT_CONFIG", cfg_file)
    if work_emptydir_path:
        merged_env.setdefault("WORK", work_emptydir_path)
    for k, v in sorted(merged_env.items()):
        env_vars.append(client.V1EnvVar(name=k, value=v))

    volume_mounts: list[client.V1VolumeMount] = []
    volumes: list[client.V1Volume] = []
    if configmap_name:
        volume_mounts.append(
            client.V1VolumeMount(
                name="job-config",
                mount_path=configmap_mount_path,
                read_only=True,
            )
        )
        volumes.append(
            client.V1Volume(
                name="job-config",
                config_map=client.V1ConfigMapVolumeSource(name=configmap_name),
            )
        )
    if work_emptydir_path:
        volume_mounts.append(
            client.V1VolumeMount(
                name="yikv-work",
                mount_path="/data",
            )
        )
        volumes.append(
            client.V1Volume(
                name="yikv-work",
                empty_dir=client.V1EmptyDirVolumeSource(),
            )
        )

    pod_spec = client.V1PodSpec(
        restart_policy="Never",
        containers=[
            client.V1Container(
                name="publish",
                image=image,
                image_pull_policy="IfNotPresent",
                env=env_vars,
                volume_mounts=volume_mounts if volume_mounts else None,
            )
        ],
        volumes=volumes if volumes else None,
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
                metadata=client.V1ObjectMeta(labels={"app.kubernetes.io/name": "yikv-pipeline-publish"}),
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
    print(f"submit_publish_job: timeout waiting for Job/{name}", file=sys.stderr)
    return 2


def main() -> int:
    p = argparse.ArgumentParser(description="Submit publish Job via Kubernetes API.")
    p.add_argument("--namespace", default="yikv")
    p.add_argument("--image", default="pipeline-worker:latest")
    p.add_argument(
        "--spec-file",
        type=Path,
        required=True,
        help="JSON file validated as PublishIndexBody (table, input, …)",
    )
    p.add_argument("--generate-name", default="yikv-publish-", help="Job metadata.generateName")
    p.add_argument("--ttl-seconds-after-finished", type=int, default=86400)
    p.add_argument("--active-deadline-seconds", type=int, default=86400)
    p.add_argument("--backoff-limit", type=int, default=0)
    p.add_argument("--service-account", default="", help="Pod serviceAccountName (optional)")
    p.add_argument("--set-env", action="append", default=[], metavar="KEY=VAL", help="Extra container env (repeatable)")
    p.add_argument(
        "--configmap",
        default="",
        metavar="NAME",
        help="Mount ConfigMap at --configmap-mount (read-only); sets SERVER_CONFIG and ARTIFACT_CONFIG "
        "to MOUNT/KEY when not already passed via --set-env",
    )
    p.add_argument(
        "--configmap-mount",
        default="/etc/yikv-job",
        help="Mount path for --configmap (default /etc/yikv-job)",
    )
    p.add_argument(
        "--configmap-key",
        default="config.json",
        help="Key inside ConfigMap for server JSON (default config.json)",
    )
    p.add_argument(
        "--work-on-emptydir",
        action="store_true",
        help="Mount ephemeral emptyDir at /data and default WORK=/data/yikvdb (unless set via --set-env)",
    )
    p.add_argument("--wait", action="store_true", help="Wait for Job completion (exit 0/1)")
    p.add_argument("--wait-timeout-sec", type=float, default=86400.0)
    args = p.parse_args()

    spec_text = args.spec_file.read_text(encoding="utf-8")
    try:
        json.loads(spec_text)
    except json.JSONDecodeError as exc:
        print(f"submit_publish_job: invalid JSON in {args.spec_file}: {exc}", file=sys.stderr)
        return 2

    extra: dict[str, str] = {}
    for item in args.set_env:
        if "=" not in item:
            print(f"submit_publish_job: bad --set-env {item!r} (want KEY=VAL)", file=sys.stderr)
            return 2
        k, v = item.split("=", 1)
        extra[k.strip()] = v

    try:
        config.load_kube_config()
    except config.ConfigException:
        try:
            config.load_incluster_config()
        except config.ConfigException as exc:
            print(f"submit_publish_job: kube config: {exc}", file=sys.stderr)
            return 2

    batch = client.BatchV1Api()
    work_empty = "/data/yikvdb" if args.work_on_emptydir else None

    job = _build_job(
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
        work_emptydir_path=work_empty,
    )
    try:
        created = batch.create_namespaced_job(args.namespace, job)
    except ApiException as exc:
        print(f"submit_publish_job: create Job failed: {exc}", file=sys.stderr)
        return 3
    name = created.metadata.name if created.metadata else ""
    print(name)
    if args.wait and name:
        return _wait_job(batch, args.namespace, name, timeout_sec=args.wait_timeout_sec)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
