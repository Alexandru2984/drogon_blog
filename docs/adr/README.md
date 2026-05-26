# Architecture Decision Records

This folder captures the load-bearing decisions a reviewer is most
likely to question: "why Drogon and not …", "why your own Sentry
client instead of sentry-native", "why a synchronous write(2) for
access logs", etc. Each ADR is a self-contained Markdown file with
the same shape:

- **Context** — what problem we were solving and what mattered.
- **Decision** — what we picked.
- **Consequences** — what we gave up, what we accept as risk.

Not every commit needs an ADR. Write one when the alternative was
plausible enough that future-you (or a new contributor) would
otherwise spend a half hour wondering why the obvious choice wasn't
made. When the answer is "because we tried both and X measurably won
on benchmark Y" or "because there's a constraint from feature Z that
isn't in the README", that's the trigger.

## Index

| ADR | Title |
|-----|-------|
| [0001](0001-drogon-framework.md)        | Drogon as the HTTP framework |
| [0002](0002-argon2id-passwords.md)      | Argon2id for password hashing |
| [0003](0003-weak-etag-derivation.md)    | Weak ETags derived from row metadata |
| [0004](0004-no-pg-bundle-in-helm.md)    | The Helm chart does not bundle PostgreSQL |
| [0005](0005-sentry-via-http.md)         | Custom HTTP Sentry client, no sentry-native |
| [0006](0006-pg-notify-over-redis-pubsub.md) | pg_notify (not Redis pub/sub) for WS fan-out |
| [0007](0007-hiredis-sync-presence.md)   | Hiredis sync API for presence, not drogon::RedisClient |
| [0008](0008-sync-write-access-log.md)   | Synchronous write(2) for access log |
| [0009](0009-readonly-grpc.md)           | gRPC surface stays read-only |
| [0010](0010-deterministic-flag-bucketing.md) | sha256(key:user_id)%100 for feature flag bucketing |
