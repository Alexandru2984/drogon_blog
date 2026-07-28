# Security

This document captures the threat model the blog is built against, the
findings of a self-audit performed in May 2026, and the controls that
mitigate each issue. It also describes how to report a vulnerability.

---

## Threat model

The blog is a multi-tenant public web application. The following actors are
assumed:

| Actor                | Capabilities                                              |
|----------------------|-----------------------------------------------------------|
| Anonymous user       | Can read posts, search, register, request password reset. |
| Authenticated user   | Can create/edit/delete *their own* content, message peers.|
| Network attacker     | Sits between client and server (mitigated by TLS).        |
| Malicious authed user| Tries to escalate to another user's data (IDOR, etc.).    |
| Credential-stuffer   | Has lists of leaked username/password pairs.              |
| Botnet               | Runs distributed brute force / scraping.                  |

Out of scope: a compromise of the VPS host itself, the PostgreSQL daemon,
the SMTP relay, or Cloudflare's edge. Those are assumed to be operated
according to current best practices.

Defense-in-depth layers already in place at the deployment level:
**Cloudflare** (DDoS / WAF), **nginx** (TLS termination, request size
caps), **fail2ban + CrowdSec** (host-level IP blocking), **ufw** (kernel
firewall).

---

## Audit findings (May 2026)

Each row was discovered by reading the code, reproduced against the running
service, and shipped with a regression test in `test/test_security.cc`
unless noted otherwise.

| # | Severity | Issue                                                                                            | Fix                                                                                                |
|---|----------|--------------------------------------------------------------------------------------------------|----------------------------------------------------------------------------------------------------|
| 1 | **High** | **User enumeration via login timing.** Argon2id verify was skipped for unknown usernames, so failed-login latency was ~2 ms vs ~120 ms for a real user. | `AuthController::loginUser` now runs Argon2id against a process-local dummy hash when the row is missing. Status code, body, and latency are uniform. |
| 2 | **High** | **Email enumeration on register.** The endpoint returned `409` if the email was taken, leaking which addresses had accounts. | `registerUser` masks email collisions: returns `201` with the normal "check your email" message, sends a `Someone tried to register with your email` notification to the legitimate owner. Username collisions still surface (usernames are public). |
| 3 | **High** | **Stale password-reset tokens.** Issuing a new reset did not invalidate prior tokens, so an old token from a stolen mailbox stayed valid. | `requestPasswordReset` issues a `DELETE FROM password_reset_tokens WHERE user_id = $1` before inserting the new row. |
| 4 | **High** | **Reset-token consumption was not atomic.** Two parallel requests with the same token both passed the `findBy` check. | `resetPassword` consumes tokens with `DELETE … WHERE token = $1 AND expires_at > NOW() RETURNING user_id`. Only one query can return rows. |
| 5 | **High** | **Email-verification consume race.** Same pattern as #4 against `users.email_verification_token`. | Single `UPDATE … SET email_verified = 1, … WHERE email_verification_token = $1 AND email_verification_expires > NOW() RETURNING id`. |
| 6 | **High** | **Session fixation.** A pre-login attacker who could plant `JSESSIONID` (e.g. via a same-site nav) kept the same SID after the victim authenticated. | `loginUser` and `logoutUser` call `session->clear()` followed by `session->changeSessionIdToClient()`, forcing Drogon to mint a fresh SID. |
| 7 | **Med**  | **Account takeover via email rewrite.** Hijacking a session let the attacker change the email and pivot via password reset. | `UserController::updateProfile` requires `current_password` whenever the email changes, sets `email_verified = 0`, and issues a new verification token. |
| 8 | **Med**  | **No per-account rate limit on login.** Credential stuffers behind rotating IPs slipped past the per-IP bucket. | Second bucket keyed by username (10 burst, 1 / 60 s refill) applied inside `loginUser`. Disabled via `BLOG_DISABLE_RATE_LIMIT=1` for integration tests. |
| 9 | **Med**  | **`resendVerification` leaked account state** with `404` (no such email) vs `400` (already verified) vs `200` (sent). | Endpoint now always responds `200` with the same body; verification mail is only sent when the underlying account exists and is unverified. |
| 10| **Med**  | **`500` on missing primary key** in `updatePost`, `deletePost`, `updateComment`, `deleteComment`, `markAsRead`, `deleteMessage`, `updateProfile`. The status code itself confirms existence for anyone running the query. | Catch `drogon::orm::UnexpectedRows` specifically and return `404` with a generic message. |
| 11| **Low**  | **CSRF cookie missing `Secure` flag.** Cookie was sent over HTTP in dev environments and would have been if TLS were ever stripped in production. | `BLOG_SECURE_COOKIES=1` (set in the prod systemd unit) flips the `Secure` flag on `csrf_token` (and existing `JSESSIONID` behaviour). |
| 12| **Low**  | **Open redirect via `?next=` after login.** The frontend pushed the raw value into the router; `//evil.example` would have leaked the user off-origin. | `LoginView.safeNext()` rejects anything that is not a single-leading-slash path. |
| 13| **Low**  | **Logout only `erased` two session keys.** The underlying session record lived on with any other keys the app might add. | `session->clear()` + `changeSessionIdToClient()`. |
| 14| **Low**  | **No backend password length floor.** The frontend asked for ≥ 8 chars but the API would accept anything non-empty. | Backend rejects `< 8` or `> 256` chars on register / reset. |
| 15| **Med**  | **XFF spoofing bypassed the per-IP rate limit.** The previous `clientIp()` took the *first* hop of `X-Forwarded-For`, but nginx's default `$proxy_add_x_forwarded_for` appends to whatever the client sent — so the first entry is user-controlled. Each spoofed header rotated the rate-limit bucket. | Prefer `X-Real-IP` (single-value, nginx-set, stripped of inbound copies), fall back to `CF-Connecting-IP`, and only when reaching XFF take the **last** hop instead of the first. Verified in `test/test_security.cc` and live against `/auth/login`. **Superseded by #17** — the waterfall this fix preserved was itself the bug: it assumed nginx stripped inbound copies of those headers, which it did not. |
| 16| **Med**  | **Cookie tossing from a sibling vhost.** `JSESSIONID` and `csrf_token` were host-scoped by default but nothing *forced* that: the blog is one of ~36 vhosts under `micutu.com`, and any of them could emit `Set-Cookie: csrf_token=x; Domain=.micutu.com`. A domain-scoped cookie shadows the host-scoped one, so a neighbouring host (or an XSS in one) controlled both halves of the double-submit pair — CSRF fully bypassed — and could likewise pin a session id. | Both cookies now carry the `__Host-` prefix whenever `BLOG_SECURE_COOKIES=1`. The prefix is browser-enforced: the cookie is rejected unless it has `Secure`, `Path=/`, and **no `Domain` attribute** — which makes it unwritable from any other host. Plain names are retained on non-TLS (dev / CI) where `__Host-` cookies are refused outright. Legacy unprefixed cookies are actively expired on the next response. See `helpers/Security.cc` and `test/test_security.cc::Security_CookieNamesUseHostPrefixUnderTls`. |
| 17| **High** | **Client-IP spoofing bypassed every per-IP rate limit.** `clientIp()` read a waterfall of `CF-Connecting-IP` → `X-Real-IP` → `X-Forwarded-For` and believed whichever appeared first, trusting them because its only peer is nginx on loopback. But nginx forwards request headers upstream verbatim unless told not to, and the vhost never stripped them — so any client could send its own `CF-Connecting-IP` and have it believed. `real_ip` was already configured host-wide, but it only rewrites `$remote_addr`; the header itself passed through untouched. A fresh value per request meant a fresh token bucket, i.e. no effective limit on `/auth/register`, `/auth/request-reset` or `/posts/search`, plus attacker-chosen entries in the access and audit logs. **Reproduced against production**: two requests carrying forged TEST-NET addresses were logged as originating from them. | Three layers. `ops/nginx/blog-proxy.conf` (included from `location /`) refuses to forward the client's copy of `CF-Connecting-IP`, `True-Client-IP`, `Forwarded` et al., and re-sets `X-Real-IP`/`X-Forwarded-For` from nginx's own `$remote_addr`; `X-Forwarded-For` is originated rather than appended, so no attacker-prefixed chain reaches the app. `ops/nginx/cloudflare-realip.conf` (http level, regenerated by `scripts/update-cloudflare-ips.sh`, drift-checked weekly by `.github/workflows/cloudflare-ips.yml`) keeps `real_ip` scoped to Cloudflare's published ranges. `clientIp()` now reads exactly one header, named by `BLOG_CLIENT_IP_HEADER` (default `X-Real-IP`), and only from a trusted peer. **Verified after the fix**: five forgery vectors all resolve to the true peer, legitimate Cloudflare traffic still resolves to the visitor's address, and five `/auth/register` attempts with five different forged IPs now 429 on the fourth. Regression test: `test/test_security.cc::Security_ClientIpOnlyTrustsProxiesAndOneHeader`. |

### Vectors verified clean

- **SQL injection.** Every dynamic query uses `$1`-style placeholders through Drogon's `execSqlAsync` or the ORM mapper. Search input flows to `websearch_to_tsquery($1)` as a bound parameter; it cannot break out into SQL syntax.
- **Stored XSS.** Post / comment / message content is rendered with Vue's text interpolation (`{{ … }}`); `v-html` is used only for `ts_headline` output, which Postgres builds server-side with only `<mark>` tags allowed.
- **IDOR on writes.** All mutation handlers (`updatePost`, `deletePost`, `updateComment`, `deleteComment`, `markAsRead`, `deleteMessage`) cross-check `session.user_id` against the row's owner before mutating.
- **Avatar path traversal.** The upload filename is generated server-side (`profile_<user_id>_<unix_ts><ext>`); the user-supplied filename is discarded except for its extension. Decompression-bomb and EXIF risks are addressed in the follow-up image-pipeline work (libvips).
- **CSRF.** Mutating endpoints require a double-submit cookie/header match (see `helpers/Security.cc`).

---

## Controls overview

```
HTTPS-only (Cloudflare + Let's Encrypt)
  │
  ├── nginx rate-limit (per other tenants; the blog itself relies on app-level)
  │
  └── Drogon application
        ├── registerSyncAdvice
        │     ├── per-IP rate limit (login, register, request-reset, resend, reset)
        │     └── CSRF double-submit check (all non-GET / non-exempt)
        │
        ├── registerPostHandlingAdvice
        │     └── CSP, HSTS, X-Frame-Options, Referrer-Policy, Permissions-Policy,
        │       Cross-Origin-Opener-Policy, Cross-Origin-Resource-Policy
        │
        ├── Session cookie       — `__Host-JSESSIONID` in prod; HttpOnly,
        │                          SameSite=Lax, Secure, Path=/, no Domain
        ├── CSRF cookie          — `__Host-csrf_token` in prod; SameSite=Lax,
        │                          Secure, readable by JS (double-submit)
        │
        ├── Argon2id (libsodium) — m=64 MiB, t=2, p=1 (OPSLIMIT_INTERACTIVE)
        │
        └── Per-user login rate limit
              — 10 burst, 1 / 60 s refill, defeats IP-rotated stuffing
```

---

## Automated security tooling

The CI pipeline runs every push through four security-relevant gates:

| Gate           | What it does                                                                                                         |
|----------------|----------------------------------------------------------------------------------------------------------------------|
| `clang-tidy`   | bugprone-*, cert-*, clang-analyzer-*, performance-*, plus a few cppcoreguidelines checks. Failures are CI errors.    |
| `cppcheck`     | warning/style/performance/portability checks over `controllers/ helpers/ main.cc`. Suppressions are explicit in `.cppcheck-suppress`. |
| ESLint         | flat-config Vue 3 + TypeScript essentials. `--max-warnings 0`, run as part of the frontend job.                       |
| Trivy          | scans the produced Docker image for `HIGH`/`CRITICAL` OS + library vulnerabilities; fails the build on any hit.       |
| Dependabot     | weekly PRs for npm (frontend + e2e), GitHub Actions, and Docker base images. Grouped by stack to keep noise low.      |

Each runs in its own job (`static-analysis`, `frontend` extended with `npm run lint`, `docker` extended with Trivy). The aggregate config lives in [`.clang-tidy`](.clang-tidy), [`.cppcheck-suppress`](.cppcheck-suppress), [`frontend_app/eslint.config.js`](frontend_app/eslint.config.js), and [`.github/dependabot.yml`](.github/dependabot.yml).

## Threat model — STRIDE summary

| Threat                  | Asset / Surface              | Primary control                                                                |
|-------------------------|------------------------------|--------------------------------------------------------------------------------|
| **S**poofing            | User identity at login       | Argon2id verify with dummy-hash branch; session ID rotated on login; SameSite=Lax, Secure, HttpOnly cookies; XFF / `X-Real-IP` trusted only behind the proxy chain. Optional 2FA (TOTP / WebAuthn / recovery codes) gates the session even after a correct password. |
| **T**ampering           | Request payloads             | All mutations require a matching CSRF double-submit; `frame-ancestors 'none'` denies clickjacking framing. |
| **R**epudiation         | Account actions              | Structured JSON access log with `req_id` per request + journald aggregation; covered for login / password reset / verify in code paths.       |
| **I**nformation disclosure | Auth flows + DB           | Email-collision masking on `/auth/register`; `/auth/login` returns identical body+timing for "no such user" vs "wrong pw"; `pg_notify` payloads stay on-host (channel is internal). |
| **D**enial of service   | Auth + uploads + DB          | Per-IP + per-username rate limiting; libvips decompression-bomb cap (≤ 6000×6000); body size limit (1 MB); DB has GIN-indexed FTS so search is sublinear. |
| **E**levation of privilege | Mutation endpoints        | Per-row owner check before update / delete on posts / comments / messages; profile email change requires `current_password`; reset token consumed atomically (`DELETE … RETURNING`). |

## Two-factor authentication

Account-takeover protection beyond the password. Three orthogonal factors,
any of which the user can enrol — they all complete the same two-step
login (`/auth/login` returns `requires_2fa: true`, then one of the
`/auth/login/verify-*` endpoints completes the session).

| Factor                | Implementation                                                                                                          |
|-----------------------|-------------------------------------------------------------------------------------------------------------------------|
| TOTP (RFC 6238)       | HMAC-SHA1, 30-second step, 6-digit codes, ±1 step verification window. Implemented in `helpers/Totp.cc` directly on top of OpenSSL HMAC + base32; no external library. Verified against the RFC 6238 Appendix B vectors in `test/test_2fa.cc`. Constant-time code comparison via `sodium_memcmp`. |
| WebAuthn passkeys     | FIDO2 with `none` attestation, supports COSE algorithms ES256 (-7) and EdDSA (-8) — together that covers ~all platform authenticators and security keys in 2026. `helpers/WebAuthn.cc` implements the minimal CBOR + COSE + signature paths against OpenSSL 3 `OSSL_PARAM` APIs. Sign-count regression is a hard reject (cloned-authenticator detection). |
| Recovery codes        | Ten single-use codes per batch, generated from a Crockford-style alphabet that skips visually ambiguous characters (no 0/O, 1/I/L). Stored as Argon2id hashes — same parameters as account passwords. A consumed code stays in the table with `used_at` set so forensics can answer "which code unlocked this account."  |

**Two-step gating.** A correct password alone never sets `session.user_id`
when 2FA is enrolled; instead the server plants `session.pending_user_id`,
returns `requires_2fa: true`, and waits for a matching factor on
`/auth/login/verify-totp`, `/auth/login/verify-recovery`, or the
`/auth/login/verify-webauthn/{begin,finish}` pair. The pending state is
attached to the session ID (which was just rotated for fixation defense
by the password step), so an off-origin attacker cannot bootstrap one.

**Disable requires re-proof.** `/auth/2fa/disable` demands both the
current password and a fresh TOTP code so a hijacked session by itself
cannot turn 2FA off.

**Per-account rate limit on verify endpoints.** Each `/auth/login/verify-*`
takes a 5-burst / 5-per-minute bucket keyed on the pending user ID,
independent of the per-IP bucket on `/auth/login`. After 5 fails the
user has to wait — defeats brute force on the 6-digit code space.

**Recovery code consumption is atomic.** A `UPDATE … SET used_at = NOW()
WHERE id = $1` on a row that already had `used_at IS NULL` filtered
during selection serialises any concurrent attempts; two parallel
requests with the same code cannot both succeed.

**Audit hooks** for 2FA events: `2fa.totp.setup`, `2fa.totp.enable`,
`2fa.totp.confirm.fail`, `2fa.disable`, `2fa.recovery.regenerate`,
`2fa.webauthn.add`, `2fa.webauthn.remove`, `2fa.verify.totp.fail`,
`2fa.verify.recovery.used`, `2fa.verify.recovery.fail`,
`2fa.verify.webauthn.ok`, `2fa.verify.webauthn.fail`, and `login.password_ok`
for the in-between state where the password was correct but 2FA still
needs to complete.

## Reporting a vulnerability

If you find something that isn't covered above, please email
`security@micutu.com` with a description, reproduction steps and (if you
have them) suggested mitigations. We aim to acknowledge within 72 hours
and fix any high-severity issue within seven days. Public disclosure is
welcome **after** a fix has shipped.
