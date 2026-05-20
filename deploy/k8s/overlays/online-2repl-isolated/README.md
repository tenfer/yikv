# Online: two replicas, **isolated** data (one PVC per pod)

This overlay replaces the single-replica `Deployment` + shared `PersistentVolumeClaim` from
`deploy/k8s/base` with a **StatefulSet** of **two** pods. Each pod receives its **own**
`ReadWriteOnce` volume via `volumeClaimTemplates` (`data-yikv-server-0`, `data-yikv-server-1`, …).

## Traffic vs data semantics

- **Headless Service** `yikv-server-hl` (`clusterIP: None`) gives each pod a **stable DNS name**:
  `yikv-server-0.yikv-server-hl.<namespace>.svc.cluster.local` (and `-1`, …). Use this when a client
  must talk to **one** specific replica and its **local** `db_path` under `/data`.
- **ClusterIP Service** `yikv-server` load-balances across **all** ready endpoints. With two
  **different** on-disk databases, that is usually **not** equivalent to one logical shard unless
  your workload treats replicas as independent partitions or read-only clones. Document routing
  explicitly for your product (per-shard clients, sticky sessions, external replication, etc.).

## Apply

From repository root (or adjust `-f` paths):

```bash
kubectl kustomize yikv-server/deploy/k8s/overlays/online-2repl-isolated | kubectl apply -f -
```

Tune `storageClassName` on the PVC template in `statefulset.yaml` if your cluster requires it.
