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
- [x] **PgBouncer sidecar / sub-deployment in Helm** — opt-in
      sidecar via `pgbouncer.enabled`; app's DB_HOST/DB_PORT flip
      to 127.0.0.1:6432 through the appDbHost/appDbPort helpers.
- [x] **Graceful shutdown + PreStop hook** — SIGTERM/SIGINT flip
      `/readyz` to 503, sleep 2s for LB notice, close all WebSockets
      with normal-close, `app().quit()` drains in-flight HTTP. Helm
      chart adds `preStop: sleep 8` + `terminationGracePeriodSeconds: 30`.
      systemd unit gets `TimeoutStopSec=30`.
- [x] **Optional CNPG dependency** — `cnpg.enabled` templates a
      hand-rolled `postgresql.cnpg.io/v1 Cluster` CR plus paired
      bootstrap Secret. Operator install is the user's job — chart
      only declares the CR.

## Feature gaps

- [x] **i18n SPA** — vue-i18n 11 with EN/RO locale files under
      `src/locales/`. LocaleSwitcher.vue persists choice to
      localStorage; initial pick falls back through stored → browser
      `navigator.language` → `en`. Navbar + auth flows + feed
      headings translated; remaining views ride on $t() with
      missing-key fallback to the literal.
- [x] **Storybook** — Storybook 10 + Vue3 + Vite. Stories for
      `PostCard`, `ToastList`, `Avatar` (the last two extracted out
      of inline templates). Frontend CI runs `build-storybook` and
      uploads the static bundle as artifact.
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

- [x] gRPC API surface — read-only `BlogReader` service
      (`GetPost`, `ListPosts`) on a second port. proto3 spec in
      `proto/blog.proto`, CMake codegen via protoc + grpc_cpp_plugin
      (gated on libgrpc++-dev). Listens via Drogon's sync DB pool
      from a dedicated thread. Helm chart exposes `grpc.enabled`
      + `grpc.port`. Mutating ops stay on REST.
- [x] WebSocket Redis adapter — `helpers/Presence` (libhiredis sync
      client) tracks online users in a shared Redis: SETEX
      `user:N:online` on WS connect, DEL on last-connection close,
      30s TTL heartbeat refresh. `/users/{id}` surfaces `online: true`
      across pods. Optional (CMake gates on libhiredis-dev;
      `BLOG_REDIS_URL` env opt-in at runtime).
- [x] A/B testing flag system — `feature_flags` table (migration
      0006 + 0007), `helpers/Flags` with snapshot-publish cache
      invalidated via `kind="flag_changed"` on the existing
      `blog_event` pg_notify channel. Deterministic bucketing
      `sha256(key:user_id) % 100`. Endpoints `/flags` + `/flags/{key}`.
      Frontend composable `useFlag` lazy-loads + re-fetches on
      auth state change.
- [x] Sentry / error tracking — `helpers/Sentry` ships a minimal
      HTTP-based ingest client (no sentry-native dep); AccessLog
      fires capture on 5xx with request context. Frontend uses
      `@sentry/vue` gated on `VITE_SENTRY_DSN`. Helm chart exposes
      `sentry.dsn` (backend) + `sentry.frontendDsn`.
