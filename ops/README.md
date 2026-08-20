# Observability

The blog ships a structured access log, a Prometheus `/metrics` endpoint,
W3C trace-context propagation, a Grafana dashboard and a starter set of
alert rules. This directory carries the bits that live outside the
application binary.

## Layout

```
ops/
  docker-compose.observability.yml   Prometheus + Grafana for local demos
  prometheus/
    prometheus.yml.example           Copy to prometheus.yml and edit
    alerts.yml                       Alert rules consumed by Prometheus
  grafana/
    provisioning/
      datasources/datasource.yml     Prometheus datasource (auto-loaded)
      dashboards/dashboards.yml      Dashboard provider (file-based)
    dashboards/
      blog-overview.json             Main service dashboard
```

## Bring up the stack locally

```bash
cd ops
cp prometheus/prometheus.yml.example prometheus/prometheus.yml
# edit prometheus.yml: paste the METRICS_TOKEN from your blog .env

docker compose -f docker-compose.observability.yml up -d
```

- Grafana → http://127.0.0.1:3000 (admin / admin), dashboard auto-loaded
  under the **Blog** folder.
- Prometheus → http://127.0.0.1:9090. Reload after edits with
  `docker kill -s HUP prometheus`.

## Metrics exposed

| Metric                                       | Type       | Purpose                                                  |
|----------------------------------------------|------------|----------------------------------------------------------|
| `blog_http_requests_total`                   | counter    | Per `route` / `method` / `status` request count          |
| `blog_http_request_duration_seconds`         | histogram  | Latency buckets, for p50 / p95 / p99 in dashboards       |
| `blog_http_requests_in_flight`               | gauge      | Currently-processing requests; saturation signal         |
| `blog_email_queue_depth`                     | gauge      | Outstanding outbound mail jobs                           |
| `blog_ws_connections`                        | gauge      | Open WebSocket subscribers                               |
| `blog_ws_connection_rejected_total`          | counter    | Sockets refused by per-session/account caps              |
| `blog_ws_policy_closure_total`                | counter    | Sockets closed for oversized/excessive controls          |
| `blog_rate_limit_buckets`                     | gauge      | Currently retained in-memory limiter keys                |
| `blog_rate_limit_bucket_capacity`             | gauge      | Hard limiter-key cardinality ceiling                     |
| `blog_rate_limit_capacity_evictions_total`    | counter    | LRU churn at the limiter ceiling                         |
| `blog_observability_input_truncated_total`    | counter    | Oversized log/trace fields or collapsed unsafe routes    |
| `blog_process_resident_memory_bytes`         | gauge      | RSS for leak / OOM watching                              |
| `blog_uptime_seconds`                        | gauge      | Time since process start                                 |
| `blog_build_info{version, git_rev}`          | gauge (1)  | Static metadata; annotate dashboards by version          |

## Tracing

The HTTP layer parses the W3C `traceparent` request header and propagates
the same header on the response. When `BLOG_TRACE_LOG=1` every sampled
request also emits a one-line JSON span to `stderr` in an OTLP-shaped
schema (`trace_id`, `span_id`, `parent_span_id`, `start_time_unix_nano`,
`end_time_unix_nano`, `attributes.{http.route, http.request.method, …}`,
`status.code`). Sampling rate is `BLOG_TRACE_SAMPLE_RATE` (default `1.0`).

`trace_id` and `span_id` are also added to each JSON access log line, so
"click a slow request in Loki → jump to its span tree in Tempo" works
once a collector like Vector or fluent-bit is in front of the service.

We deliberately do not link `opentelemetry-cpp`: pulling in protobuf +
gRPC for a small service that already exports the same shape to stderr
is the wrong cost / benefit trade-off. If a future deployment needs
push-style OTLP/HTTP export, drop a sidecar collector that tails our
stderr — no code change in the blog.

## Alerts

The starter rules in `prometheus/alerts.yml`:

| Alert                  | Fires when                                       | Severity  |
|------------------------|--------------------------------------------------|-----------|
| `BlogDown`             | `up{job="blog"} == 0` for 1m                     | critical  |
| `BlogRestartLoop`      | More than 3 uptime resets in 30m                  | critical  |
| `BlogHighErrorRate`    | 5xx ratio > 1% for 5m                            | warning   |
| `BlogHighP99Latency`   | p99 > 1s for 10m                                 | warning   |
| `BlogEmailQueueBackup` | `blog_email_queue_depth > 100` for 5m            | warning   |
| `BlogWorkerPoolShedding` | Auth/media jobs are refused                    | warning   |
| `BlogHighInFlight`     | `blog_http_requests_in_flight > 100` for 2m      | warning   |
| `BlogRateLimiterCardinalityChurn` | Limiter LRU reaches its hard ceiling | warning   |
| `BlogWebSocketPolicyAbuse` | Realtime limits reject abusive clients       | warning   |
| `BlogObservabilityInputAbuse` | Log/metric inputs exceed safe budgets       | warning   |
| `BlogMemoryGrowth`     | RSS > 1.5 GiB for 15m                            | warning   |

Tune thresholds per environment — these defaults are sized for a single
1-vCPU VPS, which is the target deployment for the public site.
