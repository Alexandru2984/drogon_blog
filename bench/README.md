# bench/

Load-test harness for the Drogon blog. Powered by [k6](https://k6.io).

## What it covers

| Scenario       | Endpoint                          | What it stresses                           |
|----------------|-----------------------------------|--------------------------------------------|
| `feed_read`    | `GET /posts`                      | Single-query feed with `LEFT JOIN users`   |
| `post_view`    | `GET /posts/{id}`                 | Same JOIN with `WHERE id = $1`             |
| `search`       | `GET /posts/search?q=…`           | `tsvector` + GIN + `ts_rank` + `ts_headline`|
| `auth_me_warm` | `GET /auth/me` (with session)     | Warm-cookie auth path — no Argon2id         |

Login/register are deliberately *not* in the matrix: Argon2id makes them slow on purpose and the per-IP rate limiter caps them before any meaningful concurrency.

## Running

```bash
# Defaults: 30 VUs, 30 s per scenario, target 127.0.0.1:8092.
./run.sh

# Tweak:
VUS=60 DURATION=60s BASE_URL=http://blog:8092 ./run.sh
```

The script:

1. **Seeds** the database with 5 users × 5 posts (idempotent — re-runs delete the previous bench rows first via SQL).
2. **Warms** Drogon's connection pool with a couple of hundred light GETs.
3. **Runs** each scenario in sequence with shared k6 thresholds (`http_req_duration p95 < 500 ms`, `error rate < 1 %`).
4. **Summarises** results into a markdown table at `results/<timestamp>/summary.md`.

## Notes

- Live rate limiting on `/auth/login` (5 burst, 5 / min) is sized for humans, not bots, so login-heavy benches need `BLOG_DISABLE_RATE_LIMIT=1` on the service.
- The bench writes to the production DB by default. Point `BASE_URL` at a `docker compose` stack if you need a clean slate.
- `summarize.py` survives small differences between k6 minor releases (v0.42+ nests trend values under `.values`, older versions store them at the top level).

## CI regression guard (`K6_STRICT=1`)

`scenarios.js` ships **two** threshold sets:

- **Default** (lax) — `p(95) < 500 ms`, `errors < 1 %`. The right call when you're running the bench locally to compare two builds — wide tolerance keeps it from flapping on a noisy laptop.
- **Strict** (`K6_STRICT=1` env) — per-scenario SLOs that fail the `perf-regression` CI job in `.github/workflows/ci.yml`. The strict numbers are tuned for the GitHub Actions `ubuntu-24.04` runner (4 vCPU, shared host); they catch order-of-magnitude regressions, not absolute parity with the prod VPS.

| Scenario        | `p(95)` ceiling | RPS floor |
|-----------------|-----------------|-----------|
| `feed_read`     | 150 ms          | 500       |
| `post_view`     | 150 ms          | 500       |
| `search`        | 300 ms          | 200       |
| `feed_read_warm`| 100 ms          | 800       |

How to invoke locally:

```bash
K6_STRICT=1 k6 run \
    -e SCENARIO=feed_read -e BASE_URL=http://127.0.0.1:8092 \
    --vus 20 --duration 20s bench/scenarios.js
```

When to update these:

- The CI runner class changes (e.g. switching to a different image / arch). The ceiling and floor scale roughly with vCPU count.
- A planned, *acknowledged* perf regression lands (e.g. you swap raw SQL for a slower abstraction and accept the tradeoff). Update the table here and the `STRICT_THRESHOLDS` block in `scenarios.js` in the same commit; don't relax silently.

The job uploads each scenario's `--summary-export` JSON as a CI artifact when it fails, so you can see exactly which metric tripped.
