# Micu's Blog

A full-stack blog platform built around a modern C++ HTTP backend and a Vue 3 SPA.

**Live:** [blog.micutu.com](https://blog.micutu.com)

> Modernized in May 2026 — migrated from SQLite to PostgreSQL, replaced
> SHA-256 password hashing with Argon2id, made SMTP non-blocking, and
> rebuilt the frontend from vanilla JS to a Vite + Vue 3 SPA.

---

## Highlights

- **C++20 backend** on the [Drogon](https://github.com/drogonframework/drogon) async HTTP framework.
- **PostgreSQL** with `GENERATED ALWAYS AS IDENTITY`, `TIMESTAMP`, and an `updated_at` trigger function applied to mutable tables.
- **Argon2id** password hashing via libsodium (`m=64 MiB, t=2, p=1`). Every hash carries its own salt + parameters; no separate salt column.
- **Non-blocking email** — the HTTP handler enqueues a `Job` into a single worker thread and returns `201` in ~135 ms; SMTP delivery (via libcurl, TLS) happens out-of-band.
- **Single-query feed** — `GET /posts` is a `LEFT JOIN posts/users` issued via `execSqlAsync`, not the N+1 pattern of `findAll + findByPrimaryKey` per row.
- **Env-driven config** — `main.cc` loads `.env`, then expands `${VAR}` placeholders inside `config.json` before handing it to Drogon. No secrets in the repo.
- **Vue 3 SPA** (TypeScript, Pinia, Vue Router in hash mode, Axios) built by Vite directly into Drogon's `document_root`.
- **Two-factor authentication** — TOTP (RFC 6238) implemented from scratch on libsodium + OpenSSL HMAC, WebAuthn passkeys (FIDO2 with ES256 + EdDSA), and Argon2id-hashed single-use recovery codes. Two-step login flow gates the session on a fresh factor even after a correct password. See [`SECURITY.md`](SECURITY.md).
- **Conditional GETs** — every cacheable `/posts*` endpoint emits a weak `ETag` derived from `(max(updated_at), count, query keys)` and honours `If-None-Match` with a header-only `304 Not Modified`. Cuts outbound bandwidth on the `/posts` feed by ~77 % for warm clients with no correctness regression (writes always change the tag). See [`BENCHMARKS.md`](BENCHMARKS.md).

## Architecture

```
                   ┌────────────────────────────┐
                   │     Vue 3 SPA (Vite)       │
                   │  frontend_app/  →  public/ │
                   └──────────────┬─────────────┘
                                  │  same-origin JSON + JSESSIONID cookie
                                  ▼
┌──────────────────────────────────────────────────────────────────┐
│                      Drogon application (C++20)                  │
│                                                                  │
│  HTTP IO loops                       DB connection pool          │
│  ────────────                        ──────────────────          │
│  AuthController  ─┐                  ┌── execSqlAsync ──┐        │
│  PostController   │  ┌─ Argon2id ─┐  │                  │        │
│  CommentController├──┤  libsodium │  ├── ORM Mapper ────┤        │
│  UserController   │  └────────────┘  │  blog_db::*      │        │
│  MessageController┘                  └──────────────────┘        │
│                                                  │               │
│   EmailHelper worker thread (queue + curl SMTP)  │               │
└──────────────────────────────────────┬───────────┼───────────────┘
                                       │           │
                              ┌────────▼──┐    ┌───▼──────────┐
                              │   SMTP    │    │ PostgreSQL 17│
                              │  relay    │    │   blog_db    │
                              └───────────┘    └──────────────┘
```

## Tech stack

| Layer       | Component                                                          |
|-------------|--------------------------------------------------------------------|
| Backend     | C++20, [Drogon](https://github.com/drogonframework/drogon) 1.9     |
| Database    | PostgreSQL 17 (IDENTITY columns, `updated_at` triggers)            |
| Auth        | libsodium Argon2id (`crypto_pwhash_str` + `_verify`)               |
| Mail        | libcurl over SMTPS, dispatched to a background worker thread       |
| Build (C++) | CMake ≥ 3.10, pkg-config                                           |
| Frontend    | Vue 3 + TypeScript + Vite + Vue Router (hash) + Pinia + Axios      |
| Reverse proxy | nginx, TLS via Let's Encrypt, fronted by Cloudflare              |
| Process mgmt  | systemd (`drogon-blog.service`)                                  |

## Quick start (Docker)

The simplest way to run the whole stack — backend, frontend, and PostgreSQL — is via Docker Compose:

```bash
cp .env.example .env          # then edit SMTP creds if you want real email
docker compose up --build
```

The app listens on `http://localhost:8092`. PostgreSQL is exposed on `5432` for local inspection.

## Manual build

### Prerequisites

```bash
sudo apt install -y \
    cmake g++ pkg-config \
    libdrogon-dev libsodium-dev libcurl4-openssl-dev libssl-dev \
    postgresql postgresql-client \
    nodejs npm
```

### Database

```bash
sudo -u postgres psql <<SQL
CREATE ROLE blog_user LOGIN PASSWORD 'change-me';
CREATE DATABASE blog_db OWNER blog_user;
GRANT ALL ON SCHEMA public TO blog_user;
SQL

DB_HOST=127.0.0.1 DB_PORT=5432 DB_NAME=blog_db DB_USER=blog_user \
DB_PASSWORD='change-me' ./migrations/apply.sh
```

Schema changes are versioned forward-only under [`migrations/`](migrations/) and tracked in an `applied_migrations` table inside the database. See [`migrations/README.md`](migrations/README.md) for the full workflow.

### Environment

Create a `.env` in the project root:

```env
DB_HOST=127.0.0.1
DB_PORT=5432
DB_NAME=blog_db
DB_USER=blog_user
DB_PASSWORD=change-me

SMTP_SERVER=smtp://smtp-relay.example.com:587
SMTP_USERNAME=blog@example.com
SMTP_PASSWORD=...
SMTP_FROM_EMAIL=blog@example.com
SMTP_FROM_NAME=Example Blog
```

### Build backend

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

### Build frontend

```bash
cd frontend_app
npm install
npm run build        # outputs to ../public/
```

### Run

```bash
./build/blog
```

## Project layout

```
drogon_blog/
├── main.cc                       # entry point; loads .env, expands ${VAR}, starts EmailHelper
├── config.json                   # Drogon config (port, DB, sessions); secrets via ${ENV}
├── migrations/                   # Versioned forward-only SQL + apply.sh runner
├── CMakeLists.txt
│
├── controllers/                  # HTTP routes
│   ├── AuthController.{h,cc}     #   /auth/*    — register/login/logout/me/verify/reset
│   ├── PostController.{h,cc}     #   /posts/*   — CRUD + likes
│   ├── CommentController.{h,cc}  #   /posts/{id}/comments, /comments/*
│   ├── UserController.{h,cc}     #   /users/*   — profile + avatar upload
│   └── MessageController.{h,cc}  #   /messages/* — private messaging
│
├── models/                       # ORM models generated by drogon_ctl
│   └── ...                       #   namespace drogon_model::blog_db::*
│
├── helpers/
│   ├── EmailHelper.h             # public interface (start/stop, send*, generateToken)
│   └── EmailHelper.cc            # worker thread + queue + libcurl SMTPS
│
├── public/                       # Drogon's document_root (Vite build output)
│   ├── index.html
│   ├── assets/                   # hashed JS/CSS chunks
│   └── uploads/                  # user-uploaded files (preserved across builds)
│
├── frontend_app/                 # Vite + Vue 3 + TypeScript SPA
│   ├── src/{api,stores,router,views,components}/
│   └── vite.config.ts            # outputs to ../public/ with emptyOutDir:false
│
└── test/                         # Drogon-test integration suite
```

## API reference

### Auth — `/auth`

| Method | Path                                | Auth     | Notes                                                                            |
|--------|-------------------------------------|----------|----------------------------------------------------------------------------------|
| POST   | `/auth/register`                    | —        | Returns 201 immediately; email is async                                          |
| POST   | `/auth/login`                       | —        | Returns `{ requires_2fa: true, methods: [...] }` if 2FA enrolled; otherwise sets `JSESSIONID` |
| POST   | `/auth/logout`                      | —        | Clears session                                                                   |
| GET    | `/auth/me`                          | ✓        | Current session user                                                             |
| POST   | `/auth/verify-email`                | —        | Body `{ token }`                                                                 |
| POST   | `/auth/request-reset`               | —        | Body `{ email }`                                                                 |
| POST   | `/auth/reset-password`              | —        | Body `{ token, password }`                                                       |
| POST   | `/auth/resend-verification`         | —        | Body `{ email }`                                                                 |
| POST   | `/auth/login/verify-totp`           | pending  | Two-step completion with `{ code }`                                              |
| POST   | `/auth/login/verify-recovery`       | pending  | Two-step completion with a recovery code                                         |
| POST   | `/auth/login/verify-webauthn/begin` | pending  | Returns challenge + allow-credentials list                                       |
| POST   | `/auth/login/verify-webauthn/finish`| pending  | Verifies the assertion; completes the session                                    |
| GET    | `/auth/2fa/status`                  | ✓        | What's enrolled (TOTP / passkeys / recovery codes remaining)                     |
| POST   | `/auth/2fa/totp/setup`              | ✓        | Returns `{ secret, otpauth_url }` for QR rendering                               |
| POST   | `/auth/2fa/totp/confirm`            | ✓        | Body `{ code }`; on success returns 10 recovery codes (one-time view)            |
| POST   | `/auth/2fa/disable`                 | ✓        | Body `{ password, totp_code }`; wipes TOTP + passkeys + recovery codes           |
| POST   | `/auth/2fa/recovery-codes/regenerate`| ✓       | Body `{ password }`; invalidates the previous batch                              |
| POST   | `/auth/2fa/webauthn/register/begin` | ✓        | Challenge for `navigator.credentials.create`                                     |
| POST   | `/auth/2fa/webauthn/register/finish`| ✓        | Verifies attestation, stores the credential                                      |
| GET    | `/auth/2fa/webauthn/list`           | ✓        | The user's passkeys                                                              |
| POST   | `/auth/2fa/webauthn/remove/{id}`    | ✓        | Delete one passkey                                                               |

### Posts — `/posts`

| Method | Path                  | Auth  | Notes                                |
|--------|-----------------------|-------|--------------------------------------|
| GET    | `/posts`              | —     | Single JOIN, ordered by created_at   |
| GET    | `/posts/{id}`         | —     | Single JOIN with author              |
| POST   | `/posts`              | ✓     |                                      |
| PUT    | `/posts/{id}`         | owner |                                      |
| DELETE | `/posts/{id}`         | owner |                                      |
| GET    | `/posts/user/{id}`    | —     |                                      |
| POST   | `/posts/{id}/like`    | ✓     |                                      |
| DELETE | `/posts/{id}/like`    | ✓     |                                      |
| GET    | `/posts/{id}/likes`   | —     |                                      |
| GET    | `/posts/search?q=`    | —     | Full-text search, ranked + highlighted |

### Comments, Users, Messages

See `controllers/CommentController.h`, `UserController.h`, `MessageController.h` — same shape.

## Markdown & cursor pagination

Posts are authored as Markdown and rendered server-side by [`cmark-gfm`](https://github.com/github/cmark-gfm) at write time. The rendered HTML is stored in `posts.content_html` so reads stay cheap. Rendering uses `CMARK_OPT_SAFE` — raw HTML is escaped, `javascript:` / `data:` / `vbscript:` URLs in links are filtered, and the GFM extensions whitelisted are tables, strikethrough, autolinks and task lists.

```jsonc
// POST /posts with content "[click](javascript:bad)"
// -> stored content_html:
"content_html": "<p><a href=\"\">click</a></p>\n"
//                          ^^ dangerous URL stripped by cmark-gfm safe mode
```

The feed is paginated via a cursor:

```
GET /posts?limit=20             -> first page
GET /posts?limit=20&before=42   -> next page, oldest known id was 42
```

`?limit` is clamped to `[1, 50]`, default `20`. The response includes `next_cursor` (smallest `id` returned, or `null` when the page didn't fill the limit). The SPA's `HomeView` uses an `IntersectionObserver` on a sentinel element to fetch the next page when the user scrolls near the bottom.

## Full-text search

`posts.search` is a `tsvector` column maintained as `GENERATED ALWAYS … STORED` from the title (weight `A`) and content (weight `B`), backed by a GIN index `idx_posts_search`. Because it's a generated column, every `INSERT`/`UPDATE` keeps the index live with no application logic and no triggers to babysit.

`GET /posts/search?q=…` builds the query with `websearch_to_tsquery('english', …)`, which tolerates raw user input (quoted phrases, `OR`, `-negate`) without throwing on punctuation. Results are ordered by `ts_rank` and snippets are produced server-side via `ts_headline` with `<mark>` highlights:

```json
{
  "query": "postgresql",
  "count": 2,
  "posts": [
    {
      "id": 4, "title": "PostgreSQL tsvector primer",
      "snippet": "body discusses GIN indexes",
      "rank": 0.6079, "author": {"id": 6, "username": "alice"}, ...
    },
    {
      "id": 5, "title": "Drogon performance tuning",
      "snippet": "title mentions <mark>postgresql</mark> once",
      "rank": 0.2432, ...
    }
  ]
}
```

The title-weighted hit is first because A > B; both posts appear in the same single round-trip via `execSqlAsync`.

## Schema

```sql
users          (id IDENTITY, username, email, password_hash, profile_image,
                bio, email_verified, email_verification_token,
                email_verification_expires, created_at, updated_at)
posts          (id IDENTITY, user_id → users, title, content, created_at, updated_at)
comments       (id IDENTITY, post_id → posts, user_id → users, content, created_at)
likes          (id IDENTITY, post_id, user_id, created_at, UNIQUE(post_id, user_id))
messages       (id IDENTITY, sender_id → users, receiver_id → users, content,
                is_read, created_at)
password_reset_tokens (id IDENTITY, user_id → users, token, expires_at, created_at)
```

`updated_at` columns on `users` and `posts` are kept current by a shared `set_updated_at()` trigger function.

## Testing

Two layers, both run in CI on every push:

```bash
# C++ integration suite — drogon-test, real Postgres, no mocks.
cmake --build build --target blog_test
TEST_DB_HOST=127.0.0.1 TEST_DB_PASSWORD=… \
  ./build/test/blog_test

# Browser end-to-end — Playwright + Chromium against the SPA + backend.
cd e2e
npm ci && npx playwright install chromium
BLOG_DISABLE_RATE_LIMIT=1 sudo systemctl restart drogon-blog
npm test
```

The C++ tests cover Argon2id verify, the auth state machine (including the constant-time login and atomic password-reset semantics from [`SECURITY.md`](SECURITY.md)), the markdown sanitiser, the cursor-paginated feed, full-text search ranking and snippets, and the image pipeline (magic-byte sniffer, EXIF strip).

The Playwright suite drives a real browser: register → login → markdown post → comment → search → logout, plus probes `/feed.xml` and `/preview/posts/{id}` directly to assert the Atom XML shape and the OpenGraph / Twitter Card meta tags. It runs in CI against a docker-compose stack with rate limiting disabled.

## Social / SEO

| Endpoint                 | What it serves                                                                       |
|--------------------------|--------------------------------------------------------------------------------------|
| `GET /feed.xml`          | Atom 1.0 feed of the 30 most recent posts, `Content-Type: application/atom+xml`.     |
| `GET /preview/posts/{id}`| Plain HTML carrying `og:title` / `og:description` / `og:url` / `twitter:card` meta tags plus a `<meta http-equiv="refresh">` that bounces human visitors to the SPA hash URL. |

Hash routing (forced by the email verification / reset links shipped to existing users) hides post-specific content from link-preview crawlers, since everything after `#` is client-side. The `/preview/...` route is the bypass: share links go there, bots get a rich snippet, real users land in the SPA on the next tick.

## Profile-image pipeline (libvips)

`POST /users/profile/image` runs every upload through `helpers/ImageProcessor`, which is built on **libvips**. The endpoint never trusts the upload's filename or MIME type:

1. The bytes are written to `uploads/tmp/profile_<id>_<ms>.upload`.
2. A magic-byte sniffer reads the first 12 bytes and only accepts JPEG / PNG / WebP — anything else (SVG, BMP, ICO, GIF, raw, arbitrary binary) returns `415 Unsupported image type`.
3. libvips opens the image with `access=sequential` so a malicious file can't keep the whole pixel buffer in memory; inputs larger than `6000×6000` are rejected with `413 Image too large` (decompression-bomb defense).
4. `VImage::thumbnail` resizes to a `256×256` square using `VIPS_INTERESTING_ATTENTION` (salience-aware center crop) so portraits land on the face.
5. Every metadata field is removed before saving — `exif-data`, `xmp-data`, `iptc-data`, `icc-profile-data`, `photoshop-data`, plus the `orientation` derivative — so GPS coordinates, camera serial numbers and embedded preview thumbnails never reach disk.
6. Output is a progressive JPEG at quality 85 with optimised Huffman tables, atomically moved into `uploads/profiles/profile_<id>_<ms>.jpg`. The DB stores the public path `/uploads/profiles/…` (served via Drogon's `document_root → public/uploads` symlink).

If processing fails after the tmp save, the half-cooked file is removed. If the DB update fails after a successful conversion, the produced JPEG is removed so the disk doesn't accumulate orphans.

## Real-time fan-out (LISTEN/NOTIFY → WebSocket)

The private-message endpoints — plus live comments on the post page — have a real-time counterpart at `ws://<host>/ws/messages` (or `wss://` in production). The handshake is gated by the same `JSESSIONID` cookie as the REST endpoints — anonymous clients are closed with code `1008` and the reason `"auth required"`.

```
              ┌──── HTTP POST /messages ─────────┐
              │     HTTP POST /posts/{id}/...    │
              ▼                                  │
            Drogon                          INSERT
              │                                  │
              │                                  ▼
              │                              Postgres
              │                                  │   AFTER INSERT trigger
              │                                  ▼
              │                          NOTIFY blog_event
              │                                  │
              │     ┌───── helpers/PgListener ◄──┘
              │     │       (dedicated libpq conn + LISTEN)
              ▼     ▼
       MessageWebSocket hub  ──────►  every subscribed client
                                       (per-user for messages,
                                        per-post for comments)
```

**Why route through the database?** The in-process push (`MessageHub.pushNewMessage(...)`) only works inside one process. By moving the fan-out trigger to a Postgres `AFTER INSERT` that calls `pg_notify('blog_event', json_build_object(...)::text)`, *every* instance of the app that's `LISTEN`-ing on the channel receives the event — so the same code scales to a horizontal deployment without sharing state between nodes. The trigger joins the sender (or comment author) inline so the payload arrives at the WS hub already enriched and no extra DB round-trip is needed.

The C++ side is `helpers/PgListener` — a single background thread that owns a dedicated libpq connection, runs `select()` on its socket, and dispatches each notification's JSON payload to a callback. The callback in `main.cc` parses `kind` and routes to either `MessageWebSocket::pushNewMessage` (one-to-one delivery, keyed by `receiver_id`) or `MessageWebSocket::pushNewComment` (broadcast to anyone who sent `{"type":"subscribe_post","post_id":X}` over the WS).

Frontend (`stores/messages.ts`) auto-connects on login, reconnects with exponential backoff (1 s → 30 s cap), keeps a per-peer conversation map plus an aggregate unread counter shown as a navbar badge, and exposes `subscribePost`/`unsubscribePost` so `PostView` can stream live comments while the user is reading. Live subscribers are exposed in Prometheus metrics as `blog_ws_connections`.

## Observability

| Endpoint   | Purpose                                                                    |
|------------|----------------------------------------------------------------------------|
| `/healthz` | Liveness probe. Always `200` once the process is up.                       |
| `/readyz`  | Readiness probe. Runs `SELECT 1` against the configured DB; `503` on fail. |
| `/metrics` | Prometheus text exposition. See below.                                     |

**Structured access log.** Every request emits a single JSON line to stdout (captured by journald in production):

```json
{"ts":"2026-05-20T18:26:10.279Z","req_id":"qGf37Qo_GXQl-tzk","trace_id":"0620ccc464116379bc12e30efba1ba04","span_id":"c01d16292c007f0e","method":"POST","path":"/auth/register","route":"/auth/register","status":201,"latency_ms":122.033,"bytes":104,"ip":"127.0.0.1"}
```

Request IDs are generated on entry (or honoured if the client supplied an `X-Request-Id` header) and echoed back in the response. `route` is the matched Drogon route pattern, so cardinality stays bounded across path parameters.

**Tracing.** The HTTP layer parses the W3C `traceparent` header on the way in and propagates it on the way out, generating a fresh trace + span when none is supplied. `trace_id` and `span_id` are stamped onto every access log line for log↔trace correlation. With `BLOG_TRACE_LOG=1` the service additionally emits an OTLP-shaped JSON span on stderr per sampled request — a collector like Vector or fluent-bit can convert these to OTLP/HTTP for Tempo / Jaeger / Honeycomb without any code change. Sampling rate is `BLOG_TRACE_SAMPLE_RATE` (default `1.0`).

**Metrics** include per-route request counters (`blog_http_requests_total{route,method,status}`), a latency histogram (`blog_http_request_duration_seconds`), an in-flight gauge (`blog_http_requests_in_flight`), the outbound email queue depth, open WebSocket subscribers, resident memory, process uptime, and a `blog_build_info{version,git_rev}` info gauge.

By default `/metrics` is reachable only from the loopback interface; set `METRICS_TOKEN=<secret>` to require `Authorization: Bearer <secret>` instead, which is what production deployments behind nginx should use.

**Dashboard + alerts.** A Grafana dashboard (`ops/grafana/dashboards/blog-overview.json`) and a set of Prometheus alert rules (`ops/prometheus/alerts.yml`) ship in this repo, together with a ready-to-run `ops/docker-compose.observability.yml` that brings up Prometheus + Grafana with everything provisioned. See [`ops/README.md`](ops/README.md) for the walk-through.

## Benchmarks

Reproducible load-test harness lives in [`bench/`](bench/) (k6 + a small Python seeder). The numbers below come from a 20 s, 30-VU run against the production binary on the same VPS that serves [`blog.micutu.com`](https://blog.micutu.com).

**Host:** Intel "Haswell" 12 vCPU · 48 GB RAM · Linux 6.14 · PostgreSQL 17 on the same node. Loopback connection; Cloudflare / nginx / TLS termination are out of the loop for these numbers.

### Read-side throughput

| Scenario                              | RPS  | p50    | p95    | p99    | errors |
|---------------------------------------|-----:|-------:|-------:|-------:|-------:|
| GET `/posts/{id}`                     | 5915 | 3.0 ms | 10.8 ms| 23.2 ms| 0.00 % |
| GET `/auth/me` (warm session)         | 5443 | 3.4 ms | 13.6 ms| 27.6 ms| 0.00 % |
| GET `/posts/search?q=…` (FTS + ts_rank)| 4406 | 4.7 ms | 15.1 ms| 27.5 ms| 0.00 % |
| GET `/posts` (feed, JOIN authors)     | 3469 | 5.5 ms | 19.7 ms| 40.8 ms| 0.00 % |

A few takeaways:

- The **single-query feed** sustains ~3.5 k req/s with p95 under 20 ms. Before the JOIN refactor this endpoint was N+1 — every post triggered an extra `findByPrimaryKey` against `users`, which would have multiplied work per request by the author cardinality.
- **Full-text search** runs faster than the feed at p50 (4.7 ms vs 5.5 ms) thanks to the GIN index on `posts.search`; `ts_headline` shows up in the tail but stays under 30 ms at p99.
- **`/auth/me` warm** has almost no DB cost — it's a single primary-key lookup and the response is a few hundred bytes — so it's mostly a measure of Drogon's HTTP path itself.

### Argon2id-bound endpoints (out of the matrix on purpose)

Login and register are intentionally slow — they spend ~120 ms running Argon2id under `OPSLIMIT_INTERACTIVE` (m=64 MiB, t=2) — and they're rate-limited (5 burst / min on `/auth/login`, 3 burst / 10 min on `/auth/register`), so concurrent load testing them isn't meaningful. Single-shot wall-clock samples on this host:

| Endpoint            | n  | median   | p95      |
|---------------------|---:|---------:|---------:|
| `POST /auth/login`  | 10 | 141 ms   | 163 ms   |
| `POST /auth/register`| 3 | 139 ms   | 141 ms   |

The cost is the security feature; the response time is independent of SMTP because the email worker is on its own thread (see [Real-time messages] for the same pattern applied to chat fan-out).

### Reproducing

```bash
# Disable rate limiting in the running service first, otherwise the
# seed phase will trip /auth/register's per-IP budget.
echo BLOG_DISABLE_RATE_LIMIT=1 >> .env
sudo systemctl restart drogon-blog

cd bench
VUS=30 DURATION=20s ./run.sh
# results land in bench/results/<UTC timestamp>/summary.md
```

## Deployment

The production deployment uses systemd + nginx:

```ini
# /etc/systemd/system/drogon-blog.service
[Service]
User=micu
WorkingDirectory=/home/micu/drogon_blog
ExecStart=/home/micu/drogon_blog/build/blog
Restart=on-failure
```

`nginx` reverse-proxies `blog.micutu.com` to `127.0.0.1:8092`; TLS via Let's Encrypt, fronted by Cloudflare.

## License

Personal project. Code is open for reading and learning; no redistribution intended.
