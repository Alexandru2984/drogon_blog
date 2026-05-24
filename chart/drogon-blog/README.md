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
  `database.port` / `database.existingSecret`. Bundling a stateful
  subchart would invite the worst-of-both-worlds: "easy to install,
  hard to operate."
- **TLS.** The Ingress template wires `tls:` straight through; pair
  with cert-manager or your own ACME setup as a separate concern.
- **Migrations.** The container's entrypoint (`docker/entrypoint.sh`)
  applies the SHA256-checked migrations under `migrations/` at boot,
  so the chart doesn't need a Job.

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
