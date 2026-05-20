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
        ├── Session cookie       — HttpOnly, SameSite=Lax, Secure (in prod)
        ├── CSRF cookie          — SameSite=Lax, Secure (in prod), readable by JS
        │
        ├── Argon2id (libsodium) — m=64 MiB, t=2, p=1 (OPSLIMIT_INTERACTIVE)
        │
        └── Per-user login rate limit
              — 10 burst, 1 / 60 s refill, defeats IP-rotated stuffing
```

---

## Reporting a vulnerability

If you find something that isn't covered above, please email
`security@micutu.com` with a description, reproduction steps and (if you
have them) suggested mitigations. We aim to acknowledge within 72 hours
and fix any high-severity issue within seven days. Public disclosure is
welcome **after** a fix has shipped.
