# e2e/

Playwright end-to-end tests for the Drogon blog. They drive a real Chromium
browser against a running instance of the SPA + backend, so they cover the
gaps the C++ integration suite can't reach: cookie handling, CSRF header
injection, route guards, markdown rendering, search highlighting, the
RSS/OG-preview endpoints.

## Running

```bash
cd e2e
npm ci
npm run install-browsers          # downloads Chromium (~150 MB, one-off)

# Default target is http://127.0.0.1:8092; override with E2E_BASE_URL.
# The service must have rate-limiting disabled for the suite to run cleanly
# — each test registers + logs in a fresh user, which trips the per-IP
# /auth/register budget otherwise.
BLOG_DISABLE_RATE_LIMIT=1 sudo systemctl restart drogon-blog
npm test
```

`workers=1` keeps the suite serial so register/login flows are deterministic
against the rate limiter even when one is active.

## Coverage

| Spec                | Scenario                                                              |
|---------------------|-----------------------------------------------------------------------|
| `auth.spec.ts`      | register → login → feed visible → logout; wrong password stays on form|
| `posts.spec.ts`     | create markdown post, render check, comment, feed ordering, search    |
| `public.spec.ts`    | `/feed.xml` shape and presence; `/preview/posts/{id}` OG/Twitter tags |
