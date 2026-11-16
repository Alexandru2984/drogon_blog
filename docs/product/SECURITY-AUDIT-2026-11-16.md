# Security review — 16 November 2026

Scope: repository at `7c02072`, running blog service, blog nginx vhost and local
listener configuration. Read-only production checks; no exploitation, load test,
user-data export or test writes against the live database. This is a source and
configuration review with focused regressions, not a penetration-test certificate.

## Findings and work log

| ID | Priority | Evidence at baseline | Action |
| --- | --- | --- | --- |
| SEC-01 | Medium | `curl -I http://127.0.0.1:8092/` returns HTML without CSP, frame protection or nosniff. `registerPostHandlingAdvice` misses static responses and sync-advice rejections. nginx supplies them on the normal vhost. | Move final response policy to pre-sending advice; test static and rejected requests. |
| SEC-02 | Medium | Pre-authentication routes are exempt from CSRF token checks; no HTTP Origin/Fetch Metadata check. SameSite does not isolate sibling vhosts. JSON content types reduce exploitability but are not a complete browser-origin policy. | Check browser Origin/Fetch Metadata on all unsafe methods, with exact configured origin matching. |
| SEC-03 | Medium | Several auth and message responses omit no-store; conversation uses private revalidation rather than prohibiting storage. Sensitive account responses need a uniform policy. | Enforce private, no-store at the final response boundary for sensitive route families. |
| SEC-04 | High review priority | Live response advertises Drogon 1.9.11; Docker pins 1.9.13. Upstream 1.9.14 documents ambiguous Content-Length/Transfer-Encoding and NUL static-path fixes. This is version exposure, not proof of an exploitable path through nginx. | Review and validate a dedicated library upgrade; do not replace libraries shared with other VPS apps. |
| SEC-05 | Medium operational | Production checkout also holds the serving frontend; building directly into public makes assets live before validation. Deployment only rolls back the binary. | Build frontend in an isolated directory before promoting verified assets; document remaining non-atomic deployment behavior. |
| SEC-06 | Medium operational | Test harness drops a schema from TEST_DB_* and interpolates environment values into shell commands. Defaults use a test DB name, but an accidental override is destructive. | Harden test target checks and shell invocation before future automated use. |

Status at initial review: remediation in progress. Results below must be updated
from actual checks; an unchecked item is not a claimed fix.

## Existing controls verified in source

Argon2id; host-prefixed secure/HttpOnly session cookies; session registry and
revocation hooks; bounded worker queues; per-IP and account login throttles;
parameterized SQL; cmark safe-mode plus DOMPurify; re-encoded image uploads with
metadata stripping and dimension bounds; published/hidden predicates on public
surfaces; WebSocket origin, message and connection limits. Presence of code is
not proof of complete protection for every interaction.

nginx forwards a single authoritative X-Real-IP and strips alternate client
claims. Application listener is loopback-only. Database and Redis listeners are
local. No host firewall changes or unrelated vhost changes are needed.

Frontend production dependency audit returned zero advisories during this review.
That result covers the registry's known advisories, not native libraries or
unknown vulnerabilities. The upstream Criteria JSON-constructor report #2576
was reviewed: this repository uses fixed column names/operators and bound values,
not user-controlled Criteria JSON. No reachable instance identified.

## Threat model and blast radius

Top threats: account/session spoofing (high impact), private message/draft
information disclosure (high impact), resource exhaustion (medium/high impact).
An application/database compromise could expose all accounts, email addresses,
password hashes, messages, drafts, session metadata and encrypted TOTP seeds.
User counts and financial loss are deliberately unquantified: this review did
not export customer data and there is no credible loss model to assign dollars.

## Detection and response

Structured access/audit logs and Prometheus rules exist. Alert delivery and
ownership have not been demonstrated; current MTTD is unknown. Suggested target:
alert within 5 minutes for readiness failure, sustained 5xx and authentication
abuse, with an operator owning delivery. Follow `ops/RUNBOOK-security-incident.md`.
A real restore and incident tabletop remain necessary operational exercises.

Vendors visible in code/config: Cloudflare, SMTP relay, optional Sentry/analytics
and optional breached-password lookup. DPAs and vendor account settings were
not inspected. Regulatory applicability and notification deadlines require an
operator/legal assessment; this review does not claim compliance certification.

## References

- OWASP CSRF prevention: https://cheatsheetseries.owasp.org/cheatsheets/Cross-Site_Request_Forgery_Prevention_Cheat_Sheet.html
- Upstream Drogon changelog: https://github.com/drogonframework/drogon/blob/master/ChangeLog.md
- Criteria JSON report: https://github.com/drogonframework/drogon/issues/2576

## Remediation validation

- SEC-01/02/03: final policy now covers static responses and explicitly covers
  sync-advice rejections (Drogon skips pre-sending advice on that path). Regression
  tests exercise static HTML, early cross-origin rejection and response headers.
- SEC-04: built private static Drogon 1.9.14 at verified revision
  `3aae769cc405cf5abb9d477675c6430924f02c7c`; upgraded Docker pin and minimum
  CMake version. CI bootstraps the same private library. No system shared library
  is replaced. The upgraded application passed all 120 backend test cases
  (1096 assertions) against a disposable PostgreSQL instance on port 55487.
- SEC-06: harness rejects names without `_test_db` suffix before connecting and
  passes command arguments directly to exec; database credentials no longer enter
  shell source. The suffix guard reduces accidental targeting; it does not replace
  keeping production credentials out of test environments.
- SEC-07 (high, defense in depth): live service's ProtectHome was disabled and
  it runs as the VPS owner's account, making personal files accessible after an
  application compromise. ProtectHome=tmpfs plus a project-only read-only bind was
  validated in a transient systemd unit: config readable, personal home and Docker
  socket inaccessible, uploads writable. Existing socket denials are retained.
- Readiness failures now log database diagnostics privately and return a generic
  message instead of database exception text.

Production rollout is recorded separately below; passing local checks does not
itself mean the live service has been upgraded.
