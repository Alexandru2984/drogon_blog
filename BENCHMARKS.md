# Benchmarks

End-to-end load test of the deployed blog at `https://blog.micutu.com`,
with a focused profiling pass and two tunings whose impact was
measured before / after on the same hardware against the same data.

## Setup

| Role | Host | CPU | RAM | OS |
|---|---|---|---|---|
| **App** (Drogon + Postgres + nginx) | OVH VPS `57.129.112.224` | 12 cores | 45 GiB | Ubuntu 25.04, kernel 6.14 |
| **Load** (k6) | Hetzner VPS `81.181.166.237` | — | — | Ubuntu 24.04 |

The load host's `/etc/hosts` overrides `blog.micutu.com → 57.129.112.224`
so traffic bypasses Cloudflare and reaches nginx on the origin directly
— the goal is to measure the app and its TLS terminator, not the CDN.
RTT bench → app: **30.4 ms** (3-packet ping average).

Corpus: 5 users × 5 posts created via the public HTTP API by
[`bench/seed.py`](bench/seed.py). Same corpus across all runs.

Driver: [k6](https://k6.io) v2.0.0, HTTP/1.1, keep-alive on. Scenarios
live in [`bench/scenarios.js`](bench/scenarios.js); the harness in
[`bench/run.sh`](bench/run.sh) seeds and runs the four scenarios.

Rate limiting was disabled for the duration of the bench via a systemd
drop-in (`BLOG_DISABLE_RATE_LIMIT=1`), then removed afterwards — the
per-IP /auth/login bucket would otherwise cap k6's `auth_me_warm` long
before any meaningful concurrency.

## Headline result (200 VUs, 30 s, origin-direct)

| Scenario       | RPS base → tuned | p50 (ms) | p95 (ms) | p99 (ms) |
|----------------|------------------|----------|----------|----------|
| `feed_read`    | 3138 → **4183** (+33%) | 51.0 → 42.1 | 141.3 → **81.5** (-42%) | 245.7 → **129.5** (-47%) |
| `post_view`    | 4357 → **5208** (+20%) | 40.4 → 34.2 | 78.3  → **51.2** (-35%) | 124.6 → **90.7**  (-27%) |
| `search`       | 3670 → **4662** (+27%) | 47.3 → 38.9 | 101.1 → **62.7** (-38%) | 167.6 → **110.6** (-34%) |
| `auth_me_warm` | 5336 → **5680** (+6%)  | 33.7 → 31.6 | 49.5  → **44.6** (-10%) | 74.3  → **57.2**  (-23%) |

All scenarios: zero failed requests across baseline and tuned runs.

## What the scenarios stress

| Scenario       | Endpoint                        | Hot path                                          |
|----------------|---------------------------------|---------------------------------------------------|
| `feed_read`    | `GET /posts`                    | Paginated list, single query with `LEFT JOIN users` |
| `post_view`    | `GET /posts/{id}`               | Same JOIN with `WHERE id = $1` (PK lookup)        |
| `search`       | `GET /posts/search?q=…`         | `tsvector` GIN + `ts_rank` + `ts_headline`        |
| `auth_me_warm` | `GET /auth/me` (cookie)         | Warm-session lookup, no Argon2id, no DB write     |

Login/register are deliberately *not* in the matrix: Argon2id is
expensive by design (`OPSLIMIT_INTERACTIVE`) and the per-IP rate
limiter is sized for humans, so a synthetic credential-stuffing load
would tell us about Argon2id's cost — which we already know — rather
than the application.

## Method

1. **Baseline** with stock config — nginx default `proxy_pass`, access
   log writing with `fwrite + fflush` under a global mutex (the
   then-current code on `main`).
2. Run the four scenarios at **200 VUs / 30 s** each, origin-direct
   from the bench VPS, exporting k6's summary JSON to
   `bench/results/origin-direct-baseline-20260524-200vu/`.
3. Apply the two tunings (next section) and **rebuild + restart**.
4. Repeat step 2 → `bench/results/origin-direct-tuned-20260524-200vu/`.

200 VUs was picked after a 30/100/200/300 ramp showed the server-side
bottleneck (not the network) emerging between 100 and 200: from 100→200
RPS roughly doubled while p95 climbed 2.7× on the baseline, classic
saturation behaviour.

## Tunings applied

### 1. nginx upstream keep-alive

Stock `proxy_pass http://127.0.0.1:8092` opens a fresh TCP connection
for every request. Under load that's a lot of `SYN/SYN-ACK/ACK/FIN`
hot loops between two processes on the same host.

```nginx
upstream blog_backend {
    server 127.0.0.1:8092;
    keepalive 64;             # idle conns to keep open per worker
    keepalive_requests 10000;
    keepalive_timeout 60s;
}
…
proxy_pass http://blog_backend;
proxy_http_version 1.1;
proxy_set_header Connection "";   # required for keepalive
```

Cost: zero. Impact: cleanly visible at ≥100 VUs (see below).

### 2. Access-log write path: drop libc buffering + global mutex

`helpers/AccessLog.cc` was writing every line through
`std::fwrite(stdout) + std::fflush(stdout)` under a `std::mutex`. The
mutex serialised all 12 IO threads on the access path, and `fflush`
issued a `write(2)` syscall after every line on top of the libc
stack-frames around `fwrite`.

Each access-log line is ≤ 1.3 KiB and ends with `\n`. journald reads
`STDOUT_FILENO` as a stream socket, and writes ≤ `PIPE_BUF` (4 KiB)
land atomically in the kernel buffer. So we can drop the libc layer
**and** the mutex and still emit one whole line per `write(2)`:

```cpp
[[maybe_unused]] auto w =
    ::write(STDOUT_FILENO, line, static_cast<std::size_t>(n));
```

Cost: one syscall instead of one syscall plus two libc indirections
plus a contended mutex. Trade-off: if journald is overwhelmed and the
return value is `< n`, we drop the line rather than block the request
path — that's the right call for an access log on the hot path. The
return is explicitly suppressed; the trace span and Prometheus
histogram (`blog_http_request_duration_seconds`) still record the
request, so observability isn't lost when a log line is dropped.

## What I tried and rejected

| Idea | Why rejected |
|---|---|
| Bump `db_clients.number_of_connections` 16 → 32 | `pg_stat_activity` showed 1 active connection at 200 VUs; queries finish in ~1.5 ms server-side. Pool wasn't the bottleneck. |
| Lower `log_level` from `DEBUG` to `INFO` | Drogon's framework log was emitting only ~4 lines per 5-minute window vs 185 k access-log lines. log_level only gates LOG_DEBUG calls inside Drogon; our access log writes via fwrite directly. |
| HTTP/2 to upstream Drogon | Drogon doesn't speak HTTP/2 as a server (yet), and nginx → 127.0.0.1 keep-alive already eliminates the handshake. |
| Brotli for dynamic responses (`use_brotli`) | Static assets already pre-compressed (`br_static: true`). JSON bodies are 100s of bytes — gzip is already on, brotli wouldn't move the needle for them. |

## Profiling notes

At 200 VUs origin-direct on the baseline:
- Drogon CPU (`pidstat` on the PID): **~330 %** (3.3 cores out of 12).
- Per-thread split (`top -H`): 10 `DbLoop` threads at 16–22 %, 12 `DrogonIo*` threads at 5–12 %. Real parallelism, just not CPU-bound.
- `blog_http_requests_in_flight` sampled mid-bench: 70–200 (Drogon is queueing requests, not stalling on them).
- nginx workers: 0 % CPU (tiny).
- Postgres backends: 2–5 % each, 9 active.

Read together: the server was queueing, not CPU-bound. The throughput
ceiling sat at the kernel/syscall boundary (per-request TCP handshake
under nginx, mutex-serialised stdio under Drogon). Both eliminated by
the tunings above.

## Loopback comparison (sanity)

To confirm the AccessLog change is real and not just a nginx side effect,
the same 30-VU loopback bench from `bench/run.sh`:

| Scenario | Loopback baseline (RPS) | Loopback tuned (RPS) |
|---|---|---|
| `feed_read`    | 4794 | 4753 |
| `post_view`    | 7369 | 7534 |
| `search`       | 5525 | 5704 |
| `auth_me_warm` | 6476 | 6415 |

Effect on loopback is within run-to-run noise (±3 %): at 30 VUs the
mutex doesn't see meaningful contention. The win compounds at high
concurrency where many IO threads would otherwise serialise on the
log mutex.

## Reproducing

```bash
# On the app host, with the service running:
BLOG_DISABLE_RATE_LIMIT=1 ./bench/run.sh                # loopback

# On the load host, with /etc/hosts → origin IP and seed.json copied:
k6 run --vus 200 --duration 30s \
       --summary-export tuned/feed_read.json \
       -e SCENARIO=feed_read -e BASE_URL=https://blog.micutu.com \
       bench/scenarios.js
```

Raw k6 JSON for both runs is checked in at
`bench/results/origin-direct-{baseline,tuned}-20260524-200vu/`.
