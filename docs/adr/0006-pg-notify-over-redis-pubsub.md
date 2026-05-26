# ADR 0006 — pg_notify (not Redis pub/sub) for WebSocket fan-out

## Context

The blog has a realtime hub at `/ws/messages` that pushes new
messages + new comments to subscribed sockets. The fan-out problem:
when one app pod's HTTP handler inserts a message, every OTHER pod
that has the receiver's socket open needs to learn about it within
~milliseconds.

The classical answer is Redis pub/sub:

```
app pod → INSERT → app code → redis PUBLISH chan, json
app pod → SUBSCRIBE chan → push to local sockets
```

We considered this but went a different way.

## Decision

Use PostgreSQL `NOTIFY` driven by `AFTER INSERT` triggers on the
`messages` + `comments` tables. Every pod runs a single
`helpers/PgListener` thread that holds a dedicated libpq connection
and LISTENs on the `blog_event` channel. The trigger fires a JSON
payload with the row identity; the listener parses it and dispatches
to `MessageWebSocket::pushNewMessage` /
`MessageWebSocket::pushNewComment`.

What tipped it:

- **No extra dependency for the core path.** A blog already runs
  Postgres. Adding Redis to the critical path doubles the "must be
  up" surface for chat to work.
- **Stronger consistency.** The trigger fires inside the same
  transaction as the INSERT. There's no window where the row
  exists but the notification got lost on its way to a separate
  message bus. With Redis, that window is real: app commits the
  INSERT, then crashes before PUBLISH.
- **One source of truth.** PG already knows about new rows. We don't
  have to define + maintain a parallel message-bus schema.

What we accepted:

- **Payload limit.** PG `NOTIFY` is capped around 8 KiB. The trigger
  for `messages` truncates content to 200 bytes + sets a
  `content_truncated` flag so the SPA refetches via REST when it
  cares. (See migration 0004.)
- **Listener delivery semantics.** PG NOTIFY is best-effort within
  a single connection's lifetime — if the LISTEN connection drops,
  notifications fired during the gap are lost. We accept that
  because losing a chat push for a few seconds during a reconnect
  is fine for this app's UX; the SPA can refetch on the next
  message arrival. For tighter delivery (e.g., financial events),
  the answer would be `LISTEN` + a journal table, not Redis.

For **presence tracking** (see ADR 0007) the tradeoff inverts:
that's a small-payload, low-write, high-read workload that fits
Redis better than the SQL trigger model.

## Consequences

- The hub trivially scales horizontally: every pod listens on the
  same channel, every pod gets every event, and each pod filters
  for sockets it owns.
- Operationally, the listener is one more long-lived libpq
  connection per pod. At 1 pod × 1 conn this is a non-issue; at
  100 pods × 1 conn that's 100 idle connections to PG, which
  PgBouncer doesn't help with (LISTEN is connection-stateful).
  At that scale we'd revisit.
- Adding a Redis pub/sub adapter is not on the roadmap. The
  `helpers/Presence` Redis usage is independent and not
  pub/sub — it's just SETEX + EXISTS.
