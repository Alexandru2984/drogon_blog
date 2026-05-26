# ADR 0007 — Hiredis sync for presence, not drogon::RedisClient

## Context

`helpers/Presence` tracks which user IDs are currently online via
a Redis key per user (`user:N:online`) with a TTL refreshed by a
heartbeat thread. It needs:

- `SETEX user:N:online 30 1` on WS connect.
- `DEL user:N:online` on last local socket close.
- `EXISTS user:N:online` for `/users/{id}` to surface `online: true`.

Drogon ships a built-in async `drogon::nosql::RedisClient` that
plays nicely with the framework's event loop. That would be the
"correct" choice on paper.

In practice, the libdrogon we link against was not built with
`USE_REDIS=ON`. Inspecting the static archive shows the
`RedisClientManager::createRedisClient` symbol is UNDEFINED in
`libdrogon.a` — the implementation requires `libhiredis` at
libdrogon's build time, and the upstream image we link against
omitted it. Switching to Drogon's RedisClient would require
rebuilding libdrogon, which is heavier than the feature warrants.

## Decision

Use the C-level `libhiredis` directly. A single shared
`redisContext*` is protected by a mutex; calls into it happen
synchronously from a small set of low-rate code paths (WS connect,
WS disconnect, periodic heartbeat, `/users/{id}` reads). Connection
loss is handled by `runCmd` doing one transparent reconnect retry.

Why this is acceptable:

- **Calls are rare.** Connect/disconnect are once-per-tab events;
  the heartbeat fires every ~15 s per pod; `/users/{id}` is
  cache-hit-dominated. A blocking 100 µs round-trip per call is in
  the noise compared to PG queries on the same handlers.
- **The mutex is uncontended.** The heartbeat thread acquires it
  for a few µs per iteration; user-driven calls are also rare.
  Real lock-contention pressure would need orders of magnitude more
  traffic than this blog will ever see.
- **The code is short.** ~250 lines including connection management,
  TTL refresh, and command dispatch. The async RedisClient
  equivalent would be longer just for the callback boilerplate.

The implementation is compiled in conditionally via
`BLOG_HAS_REDIS` (set by CMake when `pkg-config --exists hiredis`
returns true). Without hiredis, the helper compiles to no-ops and
the binary still ships — single-pod deployments don't need any of
this.

## Consequences

- **Sync-in-async hazard.** If a future caller invokes
  `presence::isOnline` from a hot path (per-request), the mutex +
  sync I/O becomes a contention surface. The header docstring calls
  this out; reviewers touching the call sites should keep the
  invocation count bounded.
- **Single connection = single point of failure.** A long-running
  Redis outage will fail every presence call until the next
  reconnect attempt succeeds. We accept that; presence degrades
  gracefully (the `online` flag is omitted, the rest of the app
  continues working). Reconnect-on-error is built into `runCmd`.
- **The `cert-dcl50-cpp` lint** fires on the `runCmd(const char*, ...)`
  C-variadic. We suppress it explicitly with a comment block — the
  variadic is deliberate because `redisvCommand` takes a `va_list`.
- **A future Drogon rebuild WITH `USE_REDIS=ON`** would let us swap
  the implementation for `drogon::nosql::RedisClient`, gaining async
  + connection pooling. The public API in `helpers/Presence.h` is
  stable enough that the swap is purely internal.
