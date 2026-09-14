# e2e/

Playwright end-to-end tests for the Drogon blog. They drive a real Chromium
browser against a running instance of the SPA + backend, so they cover the
gaps the C++ integration suite can't reach: cookie handling, CSRF header
injection, route guards, markdown rendering, search highlighting, the
RSS/OG-preview endpoints.

## Running

Never against production. Every spec registers users and writes posts and
comments, and runs against the live service are how the production database
ended up holding accounts like `2fauser_…` and `etagtest_…`. The config
refuses to start without `E2E_BASE_URL`, and refuses the live service — port
8092 on the host that runs `drogon-blog.service`, or any `*.micutu.com`
address — unless `E2E_ALLOW_PRODUCTION=1` is set. Don't set it.

Use the same disposable stack CI does: the compose app plus an override that
turns off rate limiting (each spec mints fresh users, which trips the per-IP
register budget otherwise), pins a test TOTP key, and publishes Postgres on
55432 for the specs that plant tokens directly. From the repository root:

```bash
cat > docker-compose.e2e.yml <<'EOF'
services:
  app:
    environment:
      BLOG_DISABLE_RATE_LIMIT: "1"
      BLOG_SITE_ORIGIN:        "http://localhost:8092"
      BLOG_SECURE_COOKIES:     "0"
      BLOG_TOTP_KEY:           "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
  db:
    ports:
      - "55432:5432"
EOF
DB_PASSWORD=e2e_db_password \
  docker compose -f docker-compose.yml -f docker-compose.e2e.yml up -d --build

cd e2e
npm ci
npm run install-browsers          # Chromium, ~150 MB, once
E2E_BASE_URL=http://localhost:8092 npm test

cd .. && docker compose -f docker-compose.yml -f docker-compose.e2e.yml down -v
```

The database helpers default to that stack (`127.0.0.1:55432`, password
`e2e_db_password`); override them with `E2E_DB_*`.

On the production host, port 8092 belongs to the live service, so publish the
test app somewhere else: under `app` in the override add
`ports: !override ["18092:8092"]` (Docker Compose 2.24 or newer), set
`BLOG_SITE_ORIGIN` to `http://localhost:18092`, and run with
`E2E_BASE_URL=http://localhost:18092`.

`workers=1` keeps the suite serial so register/login flows are deterministic
against the rate limiter even when one is active.

## Coverage

| Spec                | Scenario                                                              |
|---------------------|-----------------------------------------------------------------------|
| `auth.spec.ts`      | register → login → feed visible → logout; wrong password stays on form|
| `posts.spec.ts`     | create markdown post, render check, comment, feed ordering, search    |
| `public.spec.ts`    | `/feed.xml` shape and presence; `/preview/posts/{id}` OG/Twitter tags |
| `moderation-and-comments.spec.ts` | edit and delete own comments, a deleted parent keeping its replies; report → moderation queue → hidden everywhere |
