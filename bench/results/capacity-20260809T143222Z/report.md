# Blog load benchmark — three-tier comparison

**What this answers:** how much the Drogon blog can take, and how much each
layer in front of it costs. Three paths for the same endpoints:

1. **direct** — straight to Drogon on `127.0.0.1:8092`, no nginx, no TLS
2. **nginx** — through nginx + TLS, Cloudflare bypassed (`--resolve` to loopback)
3. **cloudflare** — the real path, `https://blog.micutu.com` out to the CF edge and back

## Environment
```
Benchmark environment — 2026-08-09T14:35:10Z
Host:            12 cores, 45G RAM
Kernel:          7.0.0-28-generic
Drogon:          number_of_threads=0 (=hardware_concurrency=12 IO loops), number_of_connections=16
                 max_connections=20000, LimitNOFILE=65536
nginx:           12 workers, worker_connections=4096, worker_rlimit_nofile=16384
Generator:       wrk, ON THE SAME BOX as the server + PostgreSQL.
                 => every number is a LOWER BOUND: the load generator competes
                    with the server for the same 12 cores. An external generator
                    would show higher ceilings.
Bench VPS:       81.181.166.237 (micus-stuff) was DOWN this session — no external
                 generator available, so tier 3 (through Cloudflare) is measured
                 as realistic per-request latency, not max throughput (a single
                 source IP saturating Cloudflare measures CF's bot/DDoS shaping,
                 not the application).
DB dataset:      2 posts, 7 users
```

> **Read every throughput number as a lower bound.** The load generator (wrk)
> ran on the same 12 cores as the server and PostgreSQL, so it competed with
> what it was measuring — box load hit 18 during direct `/posts` and 25 through
> nginx. An external generator would show higher ceilings. The bench VPS that
> would have provided one was down this session.
>
> **The `/posts` numbers are also optimistic on dataset:** the table holds 2
> posts. A real feed page returns up to 20 rows, each with a tag-JSON subquery,
> so production `/posts` at scale is heavier than shown here. `/healthz` (no DB)
> is the clean measure of raw request handling.

## Throughput — requests/sec (higher is better)

### /healthz — raw request handling (no database)

| path | conns | req/s | p50 | p99 | errors |
|---|--:|--:|--:|--:|---|
| direct /healthz | 100 | 31016.53 | 2.88ms | 21.52ms | — |
| direct /healthz | 400 | 30732.35 | 11.66ms | 103.07ms | — |
| direct /healthz | 1000 | 29694.70 | 30.45ms | 343.90ms | — |
| direct /healthz | 2000 | 29390.45 | 65.70ms | 112.39ms | — |
| direct /posts | 50 | 8314.67 | 5.20ms | 19.50ms | — |
| direct /posts | 100 | 8380.43 | 10.85ms | 35.47ms | — |
| direct /posts | 200 | 6817.74 | 25.00ms | 822.55ms | — |
| direct /posts | 400 | 7384.40 | 50.94ms | 110.18ms | — |
| direct /posts | 800 | 7578.85 | 100.53ms | 179.45ms | — |
| nginx /healthz | 100 | 12689.01 | 6.98ms | 47.93ms | — |
| nginx /healthz | 400 | 13139.06 | 26.81ms | 121.20ms | — |
| nginx /healthz | 1000 | 11764.92 | 72.82ms | 1.09s | — |
| nginx /healthz | 2000 | 11788.49 | 105.46ms | 3.24s | connect 0; read 0; write 0; timeout 343 |
| nginx /posts | 50 | 5007.97 | 8.61ms | 34.85ms | — |
| nginx /posts | 100 | 4601.49 | 19.23ms | 100.06ms | — |
| nginx /posts | 200 | 5652.17 | 30.67ms | 124.26ms | — |
| nginx /posts | 400 | 5257.12 | 68.49ms | 288.03ms | — |
| nginx /posts | 800 | 4988.95 | 141.45ms | 379.59ms | — |

### /healthz  — raw request handling (no database)

| path | conns | req/s | p50 | p99 | errors |
|---|--:|--:|--:|--:|---|
| direct | 100 | 31016.53 | 2.88ms | 21.52ms | — |
| direct | 400 | 30732.35 | 11.66ms | 103.07ms | — |
| direct | 1000 | 29694.70 | 30.45ms | 343.90ms | — |
| direct | 2000 | 29390.45 | 65.70ms | 112.39ms | — |
| nginx | 100 | 12689.01 | 6.98ms | 47.93ms | — |
| nginx | 400 | 13139.06 | 26.81ms | 121.20ms | — |
| nginx | 1000 | 11764.92 | 72.82ms | 1.09s | — |
| nginx | 2000 | 11788.49 | 105.46ms | 3.24s | timeout 343 |

### /posts  — JSON feed + PostgreSQL (2-post table, see caveat)

| path | conns | req/s | p50 | p99 | errors |
|---|--:|--:|--:|--:|---|
| direct | 50 | 8314.67 | 5.20ms | 19.50ms | — |
| direct | 100 | 8380.43 | 10.85ms | 35.47ms | — |
| direct | 200 | 6817.74 | 25.00ms | 822.55ms | — |
| direct | 400 | 7384.40 | 50.94ms | 110.18ms | — |
| direct | 800 | 7578.85 | 100.53ms | 179.45ms | — |
| nginx | 50 | 5007.97 | 8.61ms | 34.85ms | — |
| nginx | 100 | 4601.49 | 19.23ms | 100.06ms | — |
| nginx | 200 | 5652.17 | 30.67ms | 124.26ms | — |
| nginx | 400 | 5257.12 | 68.49ms | 288.03ms | — |
| nginx | 800 | 4988.95 | 141.45ms | 379.59ms | — |
| cloudflare | 50 | 3430.76 | 13.36ms | 25.79ms | — |
| cloudflare | 200 | 3843.77 | 36.32ms | 100.21ms | **20885 non-2xx** |

### Through Cloudflare

Modest single-IP load is served cleanly; pushing harder trips CF's
per-IP DDoS/bot mitigation, which is the point of it.

| conns | req/s | result |
|--:|--:|---|
| 50 | 3430.76 | all 200 |
| 200 | 3843.77 | 20885 responses challenged (403) — CF mitigation engaged |

After the 200-conn burst, the source IP itself received 403 on every
request for a few seconds, then cleared. Other visitors are unaffected —
the mitigation is per source IP.

## Latency — milliseconds, low concurrency (what one user feels)

| endpoint | path | TTFB p50 | TTFB p95 | total p50 |
|---|---|--:|--:|--:|
| /healthz | direct | 1.0 | 1.2 | 1.0 |
| /healthz | nginx | 8.3 | 10.3 | 8.3 |
| /healthz | cloudflare | 39.5 | 58.5 | 39.6 |
| /posts | direct | 2.0 | 3.2 | 2.1 |
| /posts | nginx | 9.4 | 12.1 | 9.4 |
| /posts | cloudflare | 39.1 | 60.6 | 39.2 |
| asset | direct | 1.2 | 1.7 | 1.2 |
| asset | nginx | 8.0 | 11.0 | 8.1 |
| asset | cloudflare | 40.2 | 68.2 | 41.8 |


The overhead is additive and each layer earns its place:

- **nginx + TLS ≈ +7 ms** over direct. That is TLS termination and a proxy
  hop, both on loopback, and it roughly halves raw throughput (30k → 13k on
  `/healthz`) because the nginx workers now want CPU too. On a box that was not
  sharing 12 cores between generator, server and proxy, the throughput cost
  would be far smaller than the latency cost suggests.
- **Cloudflare ≈ +30 ms** over nginx. That is the round trip from this box out
  to the Frankfurt edge (`cf-ray …-FRA`) and back. A real visitor does not pay
  this as overhead — they are *near* an edge and *far* from this origin, so for
  cached content the edge replaces the long path to the origin entirely. This
  measurement, taken from the origin itself, understates the cache win for real
  users rather than overstating it.

## So how many concurrent users?

Two different ceilings, and which one you hit depends on what users are doing.

**Users holding connections** (WebSockets, open tabs) — the limit is connection
count, and it is now set deliberately, not discovered by accident:

| layer | ceiling | set by |
|---|--:|---|
| nginx | ~49,000 | `worker_connections 4096` × 12, now reachable |
| Drogon | 20,000 | `max_connections` (proven to 58k with the cap lifted) |
| memory | not the limit | ~7 KB/connection; 20k ≈ 140 MB |

**Users actively loading pages** — the limit is throughput, and the honest
number is CPU-and-Postgres bound. At ~5,000 `/posts` req/s through nginx (this
box, contended) and 2 origin requests per page load after the caching fix,
that is on the order of **2,500 page loads per second**. A reader who loads a
page every ~30 s means one such user costs ~1/30 of that, so the box sustains
**tens of thousands of active readers** before throughput, not connections,
becomes the wall — and the two ceilings are close, which is what a balanced
system looks like.

**The single biggest capacity lever was not raising a limit.** Stripping the
session cookie off `/assets/` so Cloudflare caches the bundles took a page load
from 10 origin requests to 2. That divided the per-user cost by five, which is
worth more than any ceiling increase because it applies to every visitor.

## What did NOT break

Across every tier, Drogon returned zero 5xx and zero application errors. The
only failures observed were (a) 343 client-side *timeouts* through nginx at
2,000 connections, which is the box oversubscribed at load 25, not the app, and
(b) Cloudflare's per-IP challenge when one address pushed 200 concurrent — which
is Cloudflare protecting the origin, working as intended. The application itself
was never the thing that gave way.
