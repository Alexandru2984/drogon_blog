# ADR 0005 — Custom HTTP Sentry client, no sentry-native

## Context

Backend error tracking. Sentry has an official C/C++ SDK
(`sentry-native`) that ships with Crashpad / Breakpad and signal
handlers for unwinding stack traces on crash. The official
integration story is to link `libsentry`, init at startup, and
forward `LOG_ERROR` calls + signal handlers to it.

What we actually need:

- Capture 5xx responses with request context (route, method,
  status, request id, trace id, User-Agent).
- Fire-and-forget over HTTP — never block the request path on
  Sentry latency.
- Off by default, opt-in via `BLOG_SENTRY_DSN`.

What `sentry-native` would also drag in:

- A ~5 MB shared library plus dependencies.
- A crash-handling signal layer that fights for `SIGSEGV` /
  `SIGABRT` with the systemd / K8s graceful-shutdown path we
  already designed (see `main.cc::shutdownHandler`).
- An upload pipeline tied to its own thread + on-disk staging
  directory.

We don't have C++ exception-aware instrumentation in the binary
that would turn `try`/`catch` into rich Sentry events — most errors
are caught by Drogon's exception advice or surface as a 5xx without
a backtrace to ship. The crash-dump path is exactly the case
`sentry-native` is designed for, and exactly the case we don't have
the prerequisites for.

## Decision

A ~250-line helper in `helpers/Sentry.{h,cc}` that:

- Parses `BLOG_SENTRY_DSN` at startup.
- Opens a Drogon `HttpClient` bound to the ingest host.
- On 5xx (hooked from the existing post-handling advice in
  `helpers/AccessLog.cc`), POSTs a single JSON event to the legacy
  `/api/<project_id>/store/` endpoint with:
  `event_id`, ISO `timestamp`, `platform: "other"`,
  `level: error`, `tags: {route, method, status, request_id}`,
  `request: {url, method, headers.User-Agent}`,
  `extra: {trace_id}`.
- Fire-and-forget with a 5 s timeout. Sentry latency never feeds
  back into the request path.

Glitchtip uses the same DSN format + ingest endpoint shape, so
operators who don't want to feed a SaaS get a 1-line swap.

## Consequences

- **No stack traces.** We ship request context, not a crash dump.
  The C++ exception handler would have to be exception-aware to do
  better, and adding that wasn't worth the rewrite given we
  rarely see unhandled exceptions in practice.
- **No PII in the payload.** `Authorization` / `Cookie` headers are
  not included; bodies are not included. The accidental-leak
  surface is small but conscious.
- **Manual schema match.** When Sentry changes the ingest payload
  shape (rare; they're aggressive about backwards compatibility),
  we have to update the JSON builder by hand. Tradeoff for not
  taking the C SDK dependency.
- **Frontend Sentry is separate.** `@sentry/vue` is a real SDK
  install in the SPA, gated on build-time `VITE_SENTRY_DSN`. When
  the DSN is unset, dead-code elimination removes the import
  entirely from the bundle (verified).
