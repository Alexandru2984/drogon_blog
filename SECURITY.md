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

## Audit findings (May–August 2026)

The findings below came from code review, isolated integration tests and,
where explicitly stated, controlled checks against the running service.
Regression coverage lives beside the affected subsystem.

| # | Severity | Issue                                                                                            | Fix                                                                                                |
|---|----------|--------------------------------------------------------------------------------------------------|----------------------------------------------------------------------------------------------------|
| 1 | **High** | **User enumeration via login timing.** Argon2id verify was skipped for unknown usernames, so failed-login latency was ~2 ms vs ~120 ms for a real user. | `AuthController::loginUser` now runs Argon2id against a process-local dummy hash when the row is missing. Status code, body, and latency are uniform. |
| 2 | **High** | **Email enumeration on register.** The endpoint returned `409` if the email was taken, leaking which addresses had accounts. | `registerUser` masks email collisions: returns `201` with the normal "check your email" message, sends a `Someone tried to register with your email` notification to the legitimate owner. Username collisions still surface (usernames are public). |
| 3 | **High** | **Stale password-reset tokens.** Issuing a new reset did not invalidate prior tokens, so an old token from a stolen mailbox stayed valid. | `requestPasswordReset` issues a `DELETE FROM password_reset_tokens WHERE user_id = $1` before inserting the new row. |
| 4 | **High** | **Reset-token consumption was not atomic.** Two parallel requests with the same token both passed the `findBy` check; policy or DB failure could also burn a valid token, and a successful reset left stolen sessions alive. | Password policy + Argon2 run before consumption. One data-modifying CTE consumes the token, changes the password and revokes every active session as a single PostgreSQL statement; any failure rolls all three effects back. Tests cover policy rejection, single-use, relogin and two-session eviction. |
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
| 18| **Med**  | **Blocking work on the IO loops was a cheap DoS.** Drogon runs each handler inline on one of its IO loops, so a handler that blocks stalls every connection assigned to that loop, not just its own. Argon2id costs ~167 ms to hash and ~158 ms to verify on the production host — deliberately, that is the defence — and it ran on the loop for every login, registration and password re-auth. Worse in aggregate: issuing recovery codes hashes ten in a row (~1.7 s), matching a submitted recovery code is a linear scan of up to ten verifies (~1.6 s, and the *wrong*-code case is the slow one, i.e. the attacker's), and `AuthController2fa` adds 29 synchronous queries. libvips decoded and resized 8 MB uploads on the loop too. Measured with one IO loop under 48 concurrent logins: an unrelated `GET /posts` saw p95 1434 ms / max 1567 ms. | `helpers/Workers` adds two bounded thread pools — Auth (Argon2id + the synchronous 2FA queries) and Media (libvips) — kept separate so slow image jobs cannot queue in front of a login. Handlers hand their blocking body to a pool and reply from there; Drogon's response callback is safe to invoke off-loop, which the codebase already relies on for database callbacks. Backlogs are bounded and a full pool sheds with `503` + `Retry-After` rather than growing without limit, and pool depth / rejections are exported to `/metrics`. Auth defaults to 4 workers, which also caps concurrent Argon2id memory at 4 × 64 MiB. Same measurement after: p95 73.6 ms / max 85.2 ms. Tests in `test/test_workers.cc`. |
| 19| **Med**  | **A stolen session could not be revoked, and a password change did not evict one.** Drogon keeps sessions in process memory and exposes no way to enumerate or invalidate one from outside the request holding it, so a user who suspected their session was compromised had no action available but to wait out the 14-day timeout. There was also no in-account password change at all — only the emailed reset flow, which needs mailbox access the user may be racing to get ahead of. The sharper edge: even after a reset, existing sessions stayed valid. Changing a password is precisely what someone does when they believe another party is in their account, and an attacker's live session surviving it made the action useless. | `user_sessions` (migration 0010) shadows Drogon's store: a random `sid` minted at login, kept inside the session payload and recorded with IP / user-agent / timestamps. `sid` is not a credential — it names a session but authenticates nothing, which is why it is stored and returned in the clear. New endpoints `/auth/change-password`, `/auth/sessions`, `/auth/sessions/revoke`, `/auth/sessions/revoke-others`; the change-password path requires the current password and revokes every *other* session. Revocation is enforced by a **pre-routing** advice — a sync advice cannot work here, since Drogon resolves the session after sync advices run — backed by a per-process in-memory set fed by a `session_revoked` event on the existing `blog_event` channel, so the check costs a hash lookup rather than a query per request. Revoking is scoped by `user_id`, so another user's sid matches nothing and `404` covers both "no such session" and "not yours". Tests in `test/test_sessions.cc`; verified end-to-end against production. |
| 20| **Med**  | **Nothing stopped a password that was already public, and the per-account login limit did not survive a restart.** The only password rule was a length floor, so `password123` and a user's own username were both accepted. Separately, the per-account bucket that catches credential stuffing from rotating IPs lived in process memory — and this application deploys often enough that waiting for a restart was a practical way to reset it. | `helpers/PasswordPolicy` follows NIST SP 800-63B: length is the control, composition rules are deliberately *not* imposed (they push people to predictable mutations — `Password1!` clears every checkbox and sits in every dictionary), and the checks that matter are a blocklist of the passwords that dominate leaked corpora, a context check against the account's own username / email, and an optional Have I Been Pwned lookup. HIBP uses k-anonymity: only the first five hex characters of the SHA-1 leave the process, and it fails open so a third-party outage cannot stop signups. Enforced on register, reset and change. `helpers/LoginThrottle` (migration 0011) persists the consecutive-failure count on `users`; crossing the threshold pauses sign-ins for a **capped** 15 minutes and emails the owner — the cap and the notification are what keep account-scoped throttling from becoming a quiet way to lock a victim out. Tests in `test/test_password_policy.cc`; verified live, including a real HIBP rejection. |
| 21| **Med**  | **No privilege model and no way to deal with abuse.** Every account was equal: there was no moderator role, no way to take down abusive content short of editing the database by hand, and no way for a reader to report anything at all. For a public site accepting user-submitted posts, comments and direct messages, that is a gap that eventually closes itself the unpleasant way. | Migration 0012 adds `users.role` (constrained to `user`/`moderator`/`admin`), reversible hide columns on posts and comments, ban columns, and a `reports` table with a partial unique index that stops one reporter flooding the queue with the same complaint. `helpers/Roles` is the single gate: privileged routes answer **404, not 403**, to an authenticated caller without the role, so the moderation surface is not discoverable by probing. Hidden content is filtered from *every* read path — feed, single post, search, profile listing, comments, Atom feed, share preview and the gRPC reader — because a predicate missing from any one of them makes hiding cosmetic. Bans are enforced by a central pre-routing advice rather than per-handler, since fifteen mutating endpoints exist today and a new one would otherwise be silently exempt; the check reads an in-memory set kept current by a `user_ban_changed` event on `blog_event`, so it costs no query. Banning also revokes the account's sessions, and staff accounts cannot be banned through the endpoint — otherwise one compromised moderator could lock out every other moderator and the admins. Roles are granted out of band via `scripts/grant-role.sh`; there is no escalation endpoint. Tests in `test/test_moderation.cc`. |
| 22| **High** | **A malformed TOTP encryption key silently downgraded new 2FA seeds to plaintext.** The parser returned the same `nullopt` for “unset” and “invalid,” so one typo in `BLOG_TOTP_KEY` disabled encryption without stopping startup. The Helm chart did not expose the key at all. | Invalid keys now throw at the parser and write boundary; startup validates before accepting traffic, and TLS/production mode refuses a missing key. The chart accepts an external Secret (preferred), validates inline key shape, injects `BLOG_TOTP_KEY`, and refuses a TLS render without it. Encryption round-trip and invalid-key regressions live in `test/test_2fa.cc`. |
| 23| **High** | **A stolen authenticated session could add an attacker's passkey or TOTP seed.** Factor enrolment and passkey removal trusted the session alone, turning a transient session theft into durable account access. Provisioning secrets and recovery codes were also returned without an explicit no-store policy. | TOTP setup, WebAuthn registration begin, and passkey removal now require the current password, verified off the IO loop. Setup creates a session-bound, one-shot authorization that expires after ten minutes and is required by TOTP confirmation / WebAuthn finish. Sensitive 2FA status, credential, provisioning, and recovery-code responses emit `Cache-Control: private, no-store`, `Pragma: no-cache`, and `Vary: Cookie`. Failed re-authentication is audited, and the enrolment/removal paths have per-account rate limits. Integration and browser regressions cover the flows. |
| 24| **High** | **The Helm chart defaulted to two app replicas despite process-local sessions.** Requests load-balanced to a different pod lost authentication, and every pod startup globally retired the session-registry rows belonging to still-running peers. The optional HPA amplified both failures and multiplied every in-memory security rate-limit budget. | The chart now defaults to one replica and fails template rendering for `replicaCount != 1` or `autoscaling.enabled=true`. The dormant HPA manifest was removed, chart documentation no longer advertises horizontal application scaling, and CI asserts both unsafe configurations are rejected. Multi-pod support is blocked until sessions and rate limits have distributed stores. |
| 25| **High** | **Session-registry writes failed open.** Login remained successful when inserting the new `sid` into `user_sessions` failed, creating an authenticated 14-day session invisible to device lists, password-reset revocation, bans, and “sign out everywhere.” | The registry row is now a prerequisite for publishing the `sid` into either a password-only or 2FA-completed session. Failure clears and rotates the session, returns `503` with `Retry-After`, and emits `login.session_registry_fail`; `login.ok` is emitted only after the durable row exists. Test-only fault injection proves both login paths fail closed and leave no active row or authenticated cookie. |
| 26| **Med**  | **2FA factor changes were non-atomic.** Recovery-code rotation deleted the old batch and inserted ten replacements one by one; TOTP/passkey activation and “disable all” likewise used independent writes. A DB or Argon2 failure could leave no recovery codes, an enabled factor whose response said it failed, or only some factors removed. Exceptions in regeneration/disable escaped the worker without an HTTP response. | All ten hashes are prepared before mutation. TOTP activation + code issuance, first-passkey + code issuance, rotation, and deletion of every factor now each use one data-modifying PostgreSQL CTE, so the statement commits completely or rolls back completely. Every worker path catches failures and returns `500`. Test-only database faults prove old codes and all factors survive rollback, TOTP remains disabled after failed confirmation, retries succeed, and the final disable removes the full set. |
| 27| **Med**  | **A password-approved 2FA login stayed pending for the full 14-day session, and WebAuthn challenge replacement was broken.** Drogon's `Session::insert()` does not overwrite an existing key, so a second begin response carried a fresh challenge while the server retained the abandoned first one. Challenge read and erase were also separate operations, allowing two concurrent finishes to read the same assertion authorization; zero-counter passkeys had no database counter guard to close that replay window. | `helpers/TwoFactorSession` gives pending login state a ten-minute TTL and clears expired state plus its challenge atomically. Session-map updates now replace prior challenges, and finish atomically claims (reads + erases) the challenge before verification, so exactly one concurrent request can proceed even when the authenticator always reports counter zero. WebAuthn enrolment claims its password step-up and challenge in the same lock. Intermediate 2FA/challenge responses are `private, no-store`, oversized login ceremony fields are rejected before parsing, expiry emits `login.2fa.expired`, and integration plus virtual-authenticator regressions cover expiry and repeated begins. |
| 28| **High** | **Attacker-controlled realtime and limiter state could exhaust the process.** The token-bucket map swept stale entries after 4,096 keys but had no hard ceiling; a botnet keeping keys fresh grew RAM forever and forced an O(n) sweep on every new key. One authenticated session could also open sockets until Drogon's global 20,000-connection cap, while each socket accepted 128 KiB JSON controls without a message-rate budget. Recovery-code rotation exposed an eleven-Argon2 job with no endpoint-specific throttle, and login accepted credential strings beyond every registrable value before queueing expensive work. | The limiter is now a 16,384-entry, fixed-key-size LRU with collision-safe namespaces, O(1) amortized expiry/eviction, and explicit bucket/eviction metrics. Login rejects impossible credential sizes before the limiter/DB/auth pool. WebSockets allow at most 8 connections per session and 16 per account, close binary/unknown/over-2 KiB/excessive controls with 1008, budget protocol pings, accept only positive post IDs, and have transport frames capped at 4 KiB. Connection/policy rejections are sampled in logs and exported as counters. Recovery-code rotation is limited to 3/10 min/account and disable attempts to 5/10 min/account before entering the auth pool. Prometheus alerts and C++/Chromium abuse regressions cover cardinality, frames, and connection floods. |
| 29| **High** | **Long request targets could read beyond fixed observability buffers and grow metrics memory forever.** `snprintf` returns the bytes it *would* have written; both the access logger and optional trace exporter passed that larger number to `write`/`fwrite` even when their 1,280/1,024-byte stack buffers had truncated the JSON. An attacker-controlled long path could therefore disclose adjacent stack bytes to journald/trace collection or crash the process. Unmatched raw paths also became permanent Prometheus route labels, yielding an unbounded map and time-series cardinality. Long paths/User-Agents amplified Sentry payloads. | `helpers/LogSafety` now produces bounded, valid JSON fields and replaces over-budget values with stable SHA-256 forensic markers. Access and trace lines are dynamically sized and written using their actual string length; the warning-prone timestamp formatter is centralized and fixed-width. Unmatched routes collapse to `/__unmatched__`; `Metrics::observeRequest` refuses empty/over-256-byte labels and independently hard-caps the route/method/status map at 2,048 series with a fixed overflow label. Sentry fields are bounded before payload construction. A truncation counter/alert makes probing visible, while unit regressions cover megabyte fields, escape expansion, UTC shape, and both long/short high-cardinality route attempts. |
| 30| **Low**  | **The frontend build lock retained `nanoid` 3.3.17, covered by high-severity GHSA-2v37-7h3g-55p8.** Calling its custom generator with size zero could loop forever. The package arrived only through PostCSS/Vite, the application never calls that API, and build dependencies are absent from the runtime image, so direct production exploitability was low; nevertheless, a poisoned/untrusted build configuration could stall CI and the repository's mandatory High audit failed. | The deterministic lock now resolves `nanoid` 3.3.18. The sole required dependency install hook is an explicitly reviewed, exact-version `esbuild@0.28.1` allowlist entry; unexpected transitive install scripts are blocked by current npm. A clean `npm ci`, production Vue/Vite build, zero-warning ESLint run, and full `npm audit --audit-level=low` all pass with zero findings. Registry signatures validate for all 374 installed packages and 114 carry attestations. Weekly Dependabot coverage and the CI High gate remain the controls against recurrence. |
| 31| **High** | **CI and image builds executed mutable upstream inputs.** Every GitHub Action used a movable major tag, three privileged build/test jobs ran `drogonframework/drogon:latest`, base and Postgres images were tag-only, and Drogon's build fetched a tag without checking its commit. The OSV scanner also ran from `latest` as root with a read-write checkout. The OpenAPI job downloaded Spectral 6.15.0 and its transitive graph afresh; that graph now resolves a `lodash` version with two High advisories, including template code injection, while parsing pull-request-controlled specifications. Main CI inherited repository-default token permissions, and the scheduled Cloudflare job unnecessarily requested issue write access. | Actions are fixed to full 40-character commits with Dependabot-compatible version comments; OCI inputs, including the Dockerfile frontend, are fixed to manifest digests; and the Drogon tag must resolve to its reviewed commit before submodules are fetched. Workflow tokens default to `contents: read`, with the unused write grant removed. Kubeconform is SHA-256 verified. Spectral 6.16.3 and OSV Scanner 2.5.1 run from digest-pinned images as UID 65534 with a read-only root, no capabilities, no privilege escalation, read-only source mounts, and no network for Spectral. A CI regression check rejects new floating actions, images, Dockerfile frontends, or Drogon revisions. Dependabot keeps reviewed SHA/digest bumps flowing. The rebuilt production image passes Trivy 0.73.0 with zero High/Critical findings. |
| 32| **Low**  | **The container smoke check could report success after the application failed.** It invoked `/app/blog --help`, but the binary has no database-free help mode and attempted to parse the unsubstituted configuration template. The resulting parse failure was piped into `head`; without `pipefail`, the final zero status masked the application error and made an ABI/config regression look green. | The misleading execution was replaced by a database-free runtime-image contract check: it asserts UID 1000, the binary, frontend and migrations, and fails if `ldd` reports any missing shared object. The E2E job remains the authoritative full startup test against a healthy PostgreSQL instance. Actionlint and ShellCheck now pass the complete workflows without findings. |

### Vectors verified clean

- **SQL injection.** Every dynamic query uses `$1`-style placeholders through Drogon's `execSqlAsync` or the ORM mapper. Search input flows to `websearch_to_tsquery($1)` as a bound parameter; it cannot break out into SQL syntax.
- **Stored XSS.** Post / comment / message content is rendered with Vue's text interpolation (`{{ … }}`); `v-html` is used only for `ts_headline` output, which Postgres builds server-side with only `<mark>` tags allowed.
- **IDOR on writes.** All mutation handlers (`updatePost`, `deletePost`, `updateComment`, `deleteComment`, `markAsRead`, `deleteMessage`) cross-check `session.user_id` against the row's owner before mutating.
- **Avatar path traversal.** The upload filename is generated server-side (`profile_<user_id>_<unix_ts><ext>`); the user-supplied filename is discarded except for its extension. Decompression-bomb and EXIF risks are addressed in the follow-up image-pipeline work (libvips).
- **CSRF.** Mutating endpoints require a double-submit cookie/header match (see `helpers/Security.cc`).

### Known limitations

These are not defects in the current deployment. They are properties of it,
written down so that changing the deployment does not silently invalidate a
control.

- **Sessions and rate limiting are per process, in memory.** Drogon's session
  store and the token buckets in `helpers/Security.cc` are process-local. The
  Helm chart refuses `replicaCount != 1` and HPA so this limitation cannot be
  enabled accidentally. Manually scaling the Deployment would bypass that
  guard, produce intermittent logouts, and multiply every limit by the number
  of pods. **Scaling past one instance means moving both stores first.** Redis
  is already an optional dependency (`BLOG_REDIS_URL`, used by
  `helpers/Presence.cc`) and is the natural candidate, but a distributed
  limiter is a change to a security-sensitive helper and is not worth making
  speculatively. Buckets are also lost on restart; that is acceptable because
  restarts are operator-initiated and all in-memory sessions end with them.

- **`/metrics` is protected by a bearer token, not by identity.** Anyone holding
  `METRICS_TOKEN` can read it. It is additionally unreachable from outside
  (nginx returns 404, the listener binds to loopback), so the token is a second
  layer rather than the only one.

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
        ├── Bounded token buckets — 16,384-entry LRU; fixed-size hashed keys
        ├── Auth/media worker pools — bounded queues; shed with 503 on saturation
        └── WebSocket budgets — 8/session, 16/user, 2 KiB controls + 10/s
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
by the password step), expires after ten minutes, and is cleared together
with any outstanding WebAuthn challenge. An off-origin attacker therefore
cannot bootstrap one or retain a password-approved partial login indefinitely.

**WebAuthn challenges are replaceable and single-use.** Repeating a begin
request replaces the abandoned challenge, which keeps normal browser retries
usable. Finish atomically claims and removes the current challenge before
signature work; only one concurrent request can receive it. The same atomic
operation binds a passkey-enrolment challenge to its fresh-password step-up.

**Disable requires re-proof.** `/auth/2fa/disable` demands both the
current password and a fresh TOTP code so a hijacked session by itself
cannot turn 2FA off.

**Factor management requires a step-up.** Starting TOTP or passkey
enrolment and removing a passkey requires the current password. TOTP
confirmation and WebAuthn registration finish additionally require the
same session's one-shot enrolment authorization, which expires after ten
minutes. A session cookie alone therefore cannot plant a durable attacker
factor. Provisioning material and recovery codes are explicitly non-cacheable.

**Per-account rate limit on verify endpoints.** Each `/auth/login/verify-*`
takes a 5-burst / 5-per-minute bucket keyed on the pending user ID,
independent of the per-IP bucket on `/auth/login`. After 5 fails the
user has to wait — defeats brute force on the 6-digit code space.

**Recovery code consumption is atomic.** A `UPDATE … SET used_at = NOW()
WHERE id = $1` on a row that already had `used_at IS NULL` filtered
during selection serialises any concurrent attempts; two parallel
requests with the same code cannot both succeed.

**Factor-set changes are atomic.** Recovery-code hashes are all computed
before any current code is deleted. Activation of a TOTP/passkey alongside
first-code issuance, batch rotation, and removal of all factors each happen in
one PostgreSQL statement. A failed write therefore leaves the previous usable
factor set intact and returns a bounded error response rather than hanging.

**Audit hooks** for 2FA events: `2fa.enroll.reauth.fail`,
`2fa.remove.reauth.fail`, `2fa.totp.setup`, `2fa.totp.enable`,
`2fa.totp.confirm.fail`, `2fa.disable`, `2fa.recovery.regenerate`,
`2fa.webauthn.add`, `2fa.webauthn.remove`, `2fa.verify.totp.fail`,
`2fa.verify.recovery.used`, `2fa.verify.recovery.fail`,
`2fa.verify.webauthn.ok`, `2fa.verify.webauthn.fail`, and `login.password_ok`
for the in-between state where the password was correct but 2FA still
needs to complete. `login.2fa.expired` identifies partial logins that reached
their ten-minute limit. `login.session_registry_fail` identifies a rejected login
whose revocation record could not be made durable; it must be alerted on as an
authentication-dependency failure, not counted as a successful login.

## Reporting a vulnerability

If you find something that isn't covered above, please email
`security@micutu.com` with a description, reproduction steps and (if you
have them) suggested mitigations. We aim to acknowledge within 72 hours
and fix any high-severity issue within seven days. Public disclosure is
welcome **after** a fix has shipped.
