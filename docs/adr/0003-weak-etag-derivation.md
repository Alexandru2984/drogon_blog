# ADR 0003 — Weak ETags derived from row metadata

## Context

The blog wants `If-None-Match` → `304 Not Modified` on read-mostly
endpoints (`/posts`, `/posts/{id}`, `/posts/{id}/comments`,
`/auth/me`, `/users/{id}`, `/messages/conversation/{id}`). The
question is what to hash into the entity tag.

Constraints:

- **Reproducible across processes + restarts.** The same response
  on two replicas, or before and after a process recycle, must
  emit the same tag — otherwise the next request invalidates every
  cache downstream (browser, Cloudflare, intermediate proxy).
- **Mutation-sensitive.** Any data change visible in the response
  body must change the tag. False positives (tag changes when body
  didn't) are bandwidth waste; false negatives (tag unchanged when
  body did) are stale serves.
- **Cheap to compute.** Ideally the inputs are already in hand from
  the SQL we needed anyway.

Possible shapes:

| Approach | Why not |
|----------|---------|
| `md5(response_body)` | Have to render the whole body before checking If-None-Match — defeats the bandwidth win on cache hits. |
| Random per-request token | Reproducibility fails; every request looks different. |
| `Last-Modified` only | RFC 7232 weak comparison + low resolution (seconds) misses sub-second updates. |
| Row metadata (count + max(updated_at) + cursor) | Hits all three constraints. |

## Decision

Weak ETags derived from a fixed shape per endpoint, hashed with
FNV-1a 64 bits and hex-encoded inside `W/"..."`:

| Endpoint                          | ETag inputs |
|-----------------------------------|-------------|
| `GET /posts`                      | `("posts", max(updated_at), max(id), count, cursor, limit)` |
| `GET /posts/{id}`                 | `(id, updated_at)` |
| `GET /posts/{id}/comments`        | `("comments", post_id, count, max(updated_at))` |
| `GET /posts/{id}/likes`           | `(post_id, count)` |
| `GET /posts/search`               | `(q, max(updated_at over matches), count)` |
| `GET /auth/me`                    | `(id, updated_at)` + `Vary: Cookie` |
| `GET /users/{id}`                 | `(id, updated_at)` |
| `GET /messages/conversation/{id}` | `(viewer, peer, count, max(created_at), sum(is_read))` + `Vary: Cookie` |

Weak (`W/"…"`) because the JSON encoder occasionally reorders
numerically-equal floats and trims trailing zeros, and `ts_headline`
on the search endpoint is not byte-stable. Weak ETags promise
"semantically equivalent" — which is what we actually want.

We do NOT skip the DB query on cache miss. The cost we save (one
cheap pre-query + one full query on miss) is much smaller than the
bandwidth win on the hit (the 304 response is a few hundred bytes
vs several KB JSON).

## Consequences

- Adding `updated_at` to a table that doesn't track it is a hard
  prerequisite. Migration 0005 backfills `comments.updated_at` so
  the comments ETag actually captures content edits.
- Endpoints with private-per-user payloads (`/auth/me`,
  `/messages/conversation`) MUST set `Vary: Cookie` and
  `Cache-Control: private`. The `applyCacheHeaders` helper does
  this automatically once a `varyHeader` argument is passed.
- The `is_read` sum is folded into the conversation ETag because
  marking a message read is the only mutation that doesn't touch a
  timestamp on the row — without it, "you've now read this message"
  would be invisible to revalidation.
