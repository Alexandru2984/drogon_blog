# drogon-blog

Helm chart for the Drogon C++20 blog. The image is multi-arch
(`linux/amd64`, `linux/arm64`) so the same chart deploys to mixed
clusters without a values fork.

## What this chart owns

- `Deployment` running the `blog` binary, rolling update (`maxSurge: 1,
  maxUnavailable: 0`), liveness on `/healthz`, readiness on `/readyz`.
- `Service` (ClusterIP by default) on port `8092` (the only port the
  app exposes — frontend is served as static assets from `/app/public`
  by Drogon itself).
- `ConfigMap` for non-secret envs (`DB_*` minus password, `SMTP_*`
  minus password, all `BLOG_*`).
- `Secret`(s) for `DB_PASSWORD` and optionally `SMTP_PASSWORD` /
  `METRICS_TOKEN`. Bring your own with `*.existingSecret` to keep them
  out of `helm get values` output.
- `ServiceAccount` with token automount disabled — the app never talks
  to the Kubernetes API, so the in-pod token is just a stray
  credential surface.
- `Ingress`, `HorizontalPodAutoscaler`, `ServiceMonitor` — all gated
  by their own `enabled` flag, off by default. Each one is wired only
  when you opt in.

## What this chart does NOT own

- **PostgreSQL.** Production should consume a managed Postgres (CNPG,
  RDS, …); the chart only points the app at it via `database.host` /
  `database.port` / `database.existingSecret`. There IS an opt-in
  `cnpg.enabled` knob (see below) that templates a CNPG `Cluster` CR
  for dev clusters — but the operator itself is your responsibility.
- **TLS.** The Ingress template wires `tls:` straight through; pair
  with cert-manager or your own ACME setup as a separate concern.
- **Migrations.** The container's entrypoint (`docker/entrypoint.sh`)
  applies the SHA256-checked migrations under `migrations/` at boot,
  so the chart doesn't need a Job.

## DB layouts (PgBouncer / CNPG)

The chart supports three DB layouts via two independent knobs.

| `pgbouncer.enabled` | `cnpg.enabled` | Layout |
|---|---|---|
| `false` | `false` | App connects directly to `database.host` (default — managed / external PG). |
| `true`  | `false` | App connects to a `pgbouncer` sidecar on `127.0.0.1:6432`; bouncer forwards to `database.host`. |
| `false` | `true`  | App connects directly to `<release>-cnpg-rw` (CNPG Service for the chart-created Cluster). |
| `true`  | `true`  | App → pgbouncer sidecar → CNPG Service. Useful for hammering an in-cluster PG in load tests. |

The app stays oblivious to which layout is active — the `appDbHost`
/ `appDbPort` helpers in `_helpers.tpl` resolve the right values
and feed them through `configmap.yaml`.

### PgBouncer (`pgbouncer.enabled: true`)

Adds a `bitnami/pgbouncer` container to the app pod. Pool mode
defaults to `transaction` (the safe pick for Drogon's threaded
worker pool — `LISTEN/NOTIFY` runs on a dedicated PgListener
connection that isn't routed through bouncer). Tunables:

| Key | Default | Notes |
|---|---|---|
| `pgbouncer.poolMode` | `transaction` | `session` / `transaction` / `statement` |
| `pgbouncer.poolSize` | `20` | server-side connections per (DB, user) |
| `pgbouncer.maxClient` | `200` | client-side cap |
| `pgbouncer.image.tag` | `1.23.1` | pin via image tag |

Worth turning on when you have >1 app replica or a workload that
bursts above PG's `max_connections` cap.

### CNPG (`cnpg.enabled: true`)

Templates a single-instance `postgresql.cnpg.io/v1 Cluster` CR plus
a paired bootstrap Secret. Convenient for `helm install + done` on a
local kind / minikube cluster.

**Prerequisite:** the [CloudNativePG operator](https://cloudnative-pg.io)
must already be installed in the cluster. The chart only declares the
`Cluster` CR — it doesn't bring the operator with it. Without the
operator, the CR sits as a dangling spec.

| Key | Default | Notes |
|---|---|---|
| `cnpg.instances` | `1` | bump to 2+ for replication |
| `cnpg.storage`  | `5Gi` | per-instance PVC size |
| `cnpg.imageName` | `""` | override the CNPG-bundled PG image |

**Not for production**: the chart hardcodes no-backup / no-monitoring
defaults that are fine for dev but irresponsible for prod. Real prod
setups should run their own CNPG `Cluster` CR (or a managed PG) and
leave `cnpg.enabled: false`.

## Minimal install

```bash
helm install blog ./chart/drogon-blog \
  --namespace blog --create-namespace \
  --set image.tag=v0.1.0 \
  --set database.host=postgres.db.svc.cluster.local \
  --set database.password=changeme-or-use-existingSecret \
  --set app.siteOrigin=https://blog.example.com
```

For anything past a local kind cluster, replace the inlined password
with a Secret you manage:

```bash
kubectl -n blog create secret generic blog-db --from-literal=password=…
helm upgrade --install blog ./chart/drogon-blog \
  --set database.existingSecret=blog-db
```

## Values reference

| Key | Default | What it does |
|---|---|---|
| `image.repository` | `ghcr.io/alexandru2984/drogon-blog` | image repo |
| `image.tag` | `""` (→ `.Chart.appVersion`) | pin per install |
| `replicaCount` | `2` | ignored when `autoscaling.enabled` |
| `database.host` / `port` / `name` / `user` | `postgres / 5432 / blog_db / blog_user` | non-secret PG conn |
| `database.password` | `""` | required unless `database.existingSecret` is set |
| `database.existingSecret` / `existingSecretKey` | `""` / `password` | bring-your-own Secret |
| `smtp.*` | empty | empty `server` → emails logged and dropped |
| `app.siteOrigin` | `https://blog.example.com` | drives WebAuthn RP id, email links, CSRF |
| `app.secureCookies` | `true` | sets `Secure` on JSESSIONID |
| `app.metricsToken` | `""` | gates `/metrics` for non-loopback peers |
| `app.disableRateLimit` | `false` | bypass `/auth/*` token buckets — bench only |
| `service.type` / `port` | `ClusterIP / 8092` | |
| `ingress.enabled` | `false` | when true, templates a v1 Ingress |
| `autoscaling.*` | `enabled: false`, `minReplicas: 2`, `maxReplicas: 6`, `targetCPUUtilizationPercentage: 70` | HPA v2 (Resource/CPU) |
| `metrics.serviceMonitor.enabled` | `false` | requires `app.metricsToken` or `metrics.serviceMonitor.bearerToken` |
| `resources` | `{}` | set requests + limits in prod |
| `podSecurityContext` / `containerSecurityContext` | hardened defaults | non-root, capabilities dropped, seccomp `RuntimeDefault` |
| `probes.*` | `/healthz` / `/readyz` | match `helpers/Ops.cc` registrations |

## What the chart enforces

- `database.password` is `required` (helm fails with a clear message)
  unless `database.existingSecret` is set.
- `metrics.serviceMonitor.enabled` is `fail`-validated against having
  *some* bearer token defined — a ServiceMonitor against a 403'd
  `/metrics` is a silent monitoring outage waiting to happen.
- `checksum/config` and `checksum/secret` annotations on the pod
  template rotate pods whenever rendered env changes, so a `helm
  upgrade` that only touches env vars still propagates.

## CI

`.github/workflows/ci.yml` runs `helm lint`, then `helm template +
kubeconform` twice — once with defaults and once with every opt-in
feature on — so a template that only renders behind a flag (Ingress,
HPA, ServiceMonitor) still gets schema-validated on every change.
