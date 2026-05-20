| Scenario | RPS | avg | p50 | p95 | p99 | max | error rate |
|---|---:|---:|---:|---:|---:|---:|---:|
| GET /auth/me (warm session) | 5443 | 5.0 ms | 3.4 ms | 13.6 ms | 27.6 ms | 215.3 ms | nan% |
| GET /posts (feed, JOIN authors) | 3469 | 8.3 ms | 5.5 ms | 19.7 ms | 40.8 ms | 359.1 ms | nan% |
| GET /posts/{id} | 5915 | 4.5 ms | 3.0 ms | 10.8 ms | 23.2 ms | 399.5 ms | nan% |
| GET /posts/search?q=… (FTS + ts_rank) | 4406 | 6.4 ms | 4.7 ms | 15.1 ms | 27.5 ms | 276.6 ms | nan% |
