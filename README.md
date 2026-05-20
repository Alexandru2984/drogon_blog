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

PGPASSWORD='change-me' psql -h 127.0.0.1 -U blog_user -d blog_db -f schema.sql
```

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
├── schema.sql                    # PostgreSQL DDL + updated_at trigger function
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

| Method | Path                       | Auth | Notes                                   |
|--------|----------------------------|------|-----------------------------------------|
| POST   | `/auth/register`           | —    | Returns 201 immediately; email is async |
| POST   | `/auth/login`              | —    | Sets `JSESSIONID` on success            |
| POST   | `/auth/logout`             | —    | Clears session                          |
| GET    | `/auth/me`                 | ✓    | Current session user                    |
| POST   | `/auth/verify-email`       | —    | Body `{ token }`                        |
| POST   | `/auth/request-reset`      | —    | Body `{ email }`                        |
| POST   | `/auth/reset-password`     | —    | Body `{ token, password }`              |
| POST   | `/auth/resend-verification`| —    | Body `{ email }`                        |

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

```bash
cmake --build build --target blog_test
./build/test/blog_test
```

Integration tests use Drogon's `drogon-test` harness, spawn the app on a loopback port, and hit it with the framework's HTTP client. Tests target a real PostgreSQL database (`blog_test_db`, see `test/setup.sql`) rather than mocks, so SQL behaviour and trigger logic are exercised end-to-end.

## Profile-image pipeline (libvips)

`POST /users/profile/image` runs every upload through `helpers/ImageProcessor`, which is built on **libvips**. The endpoint never trusts the upload's filename or MIME type:

1. The bytes are written to `uploads/tmp/profile_<id>_<ms>.upload`.
2. A magic-byte sniffer reads the first 12 bytes and only accepts JPEG / PNG / WebP — anything else (SVG, BMP, ICO, GIF, raw, arbitrary binary) returns `415 Unsupported image type`.
3. libvips opens the image with `access=sequential` so a malicious file can't keep the whole pixel buffer in memory; inputs larger than `6000×6000` are rejected with `413 Image too large` (decompression-bomb defense).
4. `VImage::thumbnail` resizes to a `256×256` square using `VIPS_INTERESTING_ATTENTION` (salience-aware center crop) so portraits land on the face.
5. Every metadata field is removed before saving — `exif-data`, `xmp-data`, `iptc-data`, `icc-profile-data`, `photoshop-data`, plus the `orientation` derivative — so GPS coordinates, camera serial numbers and embedded preview thumbnails never reach disk.
6. Output is a progressive JPEG at quality 85 with optimised Huffman tables, atomically moved into `uploads/profiles/profile_<id>_<ms>.jpg`. The DB stores the public path `/uploads/profiles/…` (served via Drogon's `document_root → public/uploads` symlink).

If processing fails after the tmp save, the half-cooked file is removed. If the DB update fails after a successful conversion, the produced JPEG is removed so the disk doesn't accumulate orphans.

## Real-time messages (WebSocket)

The private-message endpoints have a live counterpart at `ws://<host>/ws/messages` (or `wss://` in production). The handshake is gated by the same `JSESSIONID` cookie as the REST endpoints — anonymous clients are closed with code `1008` and the reason `"auth required"`.

```
       browser ──── HTTP POST /messages ────►  Drogon
          ▲                                     │
          │                                     │ INSERT
          │                                     ▼
          │                                  Postgres
          │
          └──── ws://…/ws/messages ◄───── MessageHub.pushNewMessage(receiver, sender, msg)
```

The hub is an in-process `unordered_map<user_id, set<WebSocketConnectionPtr>>` guarded by a mutex. `MessageController::sendMessage` fans the freshly-persisted row out to:
- every open socket belonging to the receiver, and
- every open socket belonging to the sender (so additional tabs / devices see the message without polling).

The frontend store (`stores/messages.ts`) auto-connects on login, reconnects with exponential backoff (1 s → 30 s cap), and keeps a per-peer conversation map plus an aggregate unread counter shown as a navbar badge. The number of live subscribers is exposed in Prometheus metrics as `blog_ws_connections`.

## Observability

| Endpoint   | Purpose                                                                    |
|------------|----------------------------------------------------------------------------|
| `/healthz` | Liveness probe. Always `200` once the process is up.                       |
| `/readyz`  | Readiness probe. Runs `SELECT 1` against the configured DB; `503` on fail. |
| `/metrics` | Prometheus text exposition. See below.                                     |

**Structured access log.** Every request emits a single JSON line to stdout (captured by journald in production):

```json
{"ts":"2026-05-19T19:54:49.707Z","req_id":"2EfFX5Bt8f6-ehXx","method":"GET","path":"/posts","route":"/posts","status":200,"latency_ms":2.500,"bytes":12,"ip":"127.0.0.1"}
```

Request IDs are generated on entry (or honoured if the client supplied an `X-Request-Id` header) and echoed back in the response. `route` is the matched Drogon route pattern, so cardinality stays bounded across path parameters.

**Metrics** include per-route request counters (`blog_http_requests_total{route,method,status}`), a latency histogram (`blog_http_request_duration_seconds`), the in-flight email queue depth, resident memory, and process uptime.

By default `/metrics` is reachable only from the loopback interface; set `METRICS_TOKEN=<secret>` to require `Authorization: Bearer <secret>` instead, which is what production deployments behind nginx should use.

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
