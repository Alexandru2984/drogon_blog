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
