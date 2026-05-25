# Backlog (for-later directions)

Captured 2026-05-24 after D19. Order is rough impact-for-portfolio,
not deadline. Strike through when shipped.

## Visibility / docs

- [x] **OpenAPI 3.1 spec + docs UI** — `openapi/blog.openapi.yaml`
      covers 42 paths / 19 schemas; served at `/api/openapi.yaml` and
      rendered by self-hosted Redoc at `/api/docs`. Spectral linted in
      CI (`openapi` job) against `.spectral.yaml`.
- [ ] **Architecture Decision Records** under `docs/adr/` — why Drogon,
      why Argon2id, why weak ETag derivation, why no-PG-bundle in Helm,
      why per-request `write(2)` over async log queue.
- [ ] **CONTRIBUTING.md** — build / test workflow, branch + commit
      conventions, link to CI matrix.

## Tests

- [x] **E2E expansion** (`e2e/`): 4 new spec files covering TOTP
      enrol + login + recovery, WebAuthn with virtual authenticator,
      password reset via DB-planted token, messaging WebSocket
      bidirectional with two browser contexts. CI wires DB port
      55432 through docker-compose.e2e.yml.
- [ ] **Perf regression guard in CI** — k6 with thresholds on loopback
      (e.g. `p95 < 50 ms`, `RPS > 4000` for `feed_read`). Defends the
      D16 / D17 numbers from silent regression.

## Operational

- [x] **Backup + restore** — `pg_dump --format=custom` daily via
      `drogon-blog-backup.timer`, rolling retention (7d / 4w / 6m),
      CI round-trip job in `backup-restore`, runbook in
      `ops/RUNBOOK-backup-restore.md`.
- [ ] **PgBouncer sidecar / sub-deployment in Helm** — connection
      pooling layer; the chart currently assumes app talks straight
      to Postgres.
- [ ] **Graceful shutdown + PreStop hook** — drain in-flight requests
      cleanly on rolling K8s deploys.
- [ ] **Optional CNPG dependency** in the Helm chart (off by default)
      — gives "helm install + done" for dev clusters without bundling
      stateful infra into the prod path.

## Feature gaps

- [ ] **i18n SPA** with vue-i18n (RO / EN switcher).
- [ ] **Storybook** for the Vue components in isolation.
- [x] **ETag on `/comments`, `/users/{id}`, `/auth/me`** — done.
      `/auth/me` emits `Vary: Cookie` + `private` cache-control;
      migration 0005 added `comments.updated_at` for cache keying.
- [ ] **`Link` headers** (RFC 5988) for cursor pagination on `/posts`
      so generic clients don't have to crack open the JSON to follow
      `next_cursor`.
- [ ] **`X-RateLimit-*` response headers** on auth endpoints so polite
      clients can self-pace before they hit 429.

## Stretch (probably overkill)

- [ ] gRPC API surface in parallel with REST.
- [ ] WebSocket Redis adapter for horizontal scaling beyond a single node
      (PgListener fan-out already covers the inserts path; presence
      counters would still split across nodes).
- [ ] A/B testing flag system.
- [ ] Sentry / error tracking integration.
