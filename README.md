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

### Comments, Users, Messages

See `controllers/CommentController.h`, `UserController.h`, `MessageController.h` — same shape.

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

## Performance notes

- **Read feed (`GET /posts`)** — one round-trip via `execSqlAsync`, no per-row author lookup. Drogon's connection pool is sized at 16 for default workloads.
- **Register (`POST /auth/register`)** — ~135 ms median; dominated by the deliberate Argon2id cost (`OPSLIMIT_INTERACTIVE`). SMTP latency is removed from the response path by the worker thread.
- **Login (`POST /auth/login`)** — ~120 ms median, same Argon2id cost (`crypto_pwhash_str_verify`).

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
