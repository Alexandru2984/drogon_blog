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
- [x] **Perf regression guard in CI** — k6 strict mode
      (`K6_STRICT=1`) with per-scenario SLOs sized for the
      ubuntu-24.04 runner profile. New `perf-regression` CI job runs
      feed_read / post_view / search and fails on threshold breach;
      summary JSON uploaded as artifact on failure.

## Operational

- [x] **Backup + restore** — `pg_dump --format=custom` daily via
      `drogon-blog-backup.timer`, rolling retention (7d / 4w / 6m),
      CI round-trip job in `backup-restore`, runbook in
      `ops/RUNBOOK-backup-restore.md`.
- [ ] **PgBouncer sidecar / sub-deployment in Helm** — connection
      pooling layer; the chart currently assumes app talks straight
      to Postgres.
- [x] **Graceful shutdown + PreStop hook** — SIGTERM/SIGINT flip
      `/readyz` to 503, sleep 2s for LB notice, close all WebSockets
      with normal-close, `app().quit()` drains in-flight HTTP. Helm
      chart adds `preStop: sleep 8` + `terminationGracePeriodSeconds: 30`.
      systemd unit gets `TimeoutStopSec=30`.
- [ ] **Optional CNPG dependency** in the Helm chart (off by default)
      — gives "helm install + done" for dev clusters without bundling
      stateful infra into the prod path.

## Feature gaps

- [ ] **i18n SPA** with vue-i18n (RO / EN switcher).
- [ ] **Storybook** for the Vue components in isolation.
- [x] **ETag on `/comments`, `/users/{id}`, `/auth/me`** — done.
      `/auth/me` emits `Vary: Cookie` + `private` cache-control;
      migration 0005 added `comments.updated_at` for cache keying.
- [x] **`Link` headers** (RFC 5988) — `/posts` emits
      `Link: </posts?cursor=N&limit=M>; rel="next"` when more
      pages exist.
- [x] **`X-RateLimit-*` response headers** — `Limit`, `Remaining`,
      `Reset` (seconds) on every rate-limited path (auth + search),
      surfaced both on 200 and 429 so polite clients can self-pace.
- [x] **ETag on `/messages/conversation`** — weak ETag from
      `(viewer, peer, count, max(created_at), sum(is_read))` with
      `Vary: Cookie` + `private` cache-control; matches the
      pattern set by `/auth/me`.

## Stretch (probably overkill)

- [ ] gRPC API surface in parallel with REST.
- [ ] WebSocket Redis adapter for horizontal scaling beyond a single node
      (PgListener fan-out already covers the inserts path; presence
      counters would still split across nodes).
- [ ] A/B testing flag system.
- [ ] Sentry / error tracking integration.
