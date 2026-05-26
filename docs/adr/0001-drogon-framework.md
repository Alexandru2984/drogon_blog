# ADR 0001 — Drogon as the HTTP framework

## Context

The blog backend is a personal-portfolio project with a few non-
negotiables:

- **Native code, not interpreted.** "Boring web app in C++" is the
  premise; using Express / Django / Rails would erase the point.
- **Coroutines + a real DB driver.** I want `co_await` on PG calls,
  not callback chains.
- **WebSockets with a registry I can poke from the same process.**
  Real-time chat without an external Redis layer.
- **Static binary footprint suitable for a ~$5 VPS.** No JVM, no
  large runtimes.

Plausible alternatives surveyed:

| Framework | Why not |
|-----------|---------|
| Beast (Boost.Beast) | Just the protocol layer; routing, JSON, ORM, sessions, WS hub all hand-rolled. Months of plumbing before the first endpoint. |
| Pistache | Lower-level, no ORM, no WS hub. Same scope creep as Beast. |
| Crow / cinatra | Comparable to Drogon's surface but smaller ecosystems; no first-class coroutine ORM at the time of evaluation. |
| oat++ | Strong on REST scaffolding but the WS story is bolted on and the ORM is opinionated about JSON-tagged DTOs. |
| Rolling our own | A larger amount of code in service of less, not more. |

## Decision

Drogon 1.9.x.

What tipped it:

- First-class coroutine ORM that round-trips Postgres via libpq with
  prepared-statement caching.
- Built-in WebSocketController with a per-connection context — the
  hub in `controllers/MessageWebSocket.cc` is ~250 lines on top.
- Session middleware with cookie configuration we needed for
  `Secure` + `HttpOnly` + `SameSite=Lax`.
- The `registerSyncAdvice` / `registerPostHandlingAdvice` hooks are
  the right abstraction for rate-limiting, CSRF, and response-header
  injection — implementing those middleware patterns ourselves would
  have been the bulk of the security work.
- Active maintenance, ~10k stars, used in production at scale by at
  least one Chinese mega-vendor (Tencent).

## Consequences

- We're coupled to Drogon's release cadence. The codebase pins
  against the API of 1.9.x; an upgrade to 2.x will need a sweep.
- A few features (gRPC, Redis pub/sub) needed to be added alongside
  Drogon rather than through it — Drogon's RedisClient required a
  drogon rebuild against libhiredis, and there's no built-in gRPC
  server. Handled via parallel listeners (`helpers/GrpcServer`,
  `helpers/Presence` going direct to libhiredis).
- Drogon's documentation is uneven — the project compensated with
  source diving (`/usr/local/include/drogon/*.h`) more than the
  README suggests would be necessary.
