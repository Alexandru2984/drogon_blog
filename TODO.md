# Backlog (for-later directions)

Captured 2026-05-24 after D19. Order is rough impact-for-portfolio,
not deadline. Strike through when shipped.

## Visibility / docs

- [ ] **OpenAPI 3.1 spec + docs UI** — schema for every endpoint, served
      at `/api/docs` (Scalar or Redoc). Validate in CI with spectral.
      Biggest "I get the API in 30 s" win.
- [ ] **Architecture Decision Records** under `docs/adr/` — why Drogon,
      why Argon2id, why weak ETag derivation, why no-PG-bundle in Helm,
      why per-request `write(2)` over async log queue.
- [ ] **CONTRIBUTING.md** — build / test workflow, branch + commit
      conventions, link to CI matrix.

## Tests

- [ ] **E2E expansion** (`e2e/`): TOTP enroll + login, recovery code,
      WebAuthn cycle with Playwright's virtual authenticator,
      password-reset full flow, messaging WebSocket bidirectional.
      Currently smoke-only.
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
- [ ] **ETag on `/comments`, `/users/{id}`, `/auth/me`** — last one
      requires `Vary: Cookie` first so a future Cloudflare cache
      doesn't serve one user's state to another.
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
