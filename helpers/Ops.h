#pragma once

// Registers operational HTTP routes:
//   GET /healthz  → liveness, always 200 if the process is alive
//   GET /readyz   → readiness, checks a DB round-trip
//   GET /metrics  → Prometheus text exposition; gated by METRICS_TOKEN env var
//                   (Bearer auth) when set, otherwise only loopback peers are
//                   accepted.
namespace ops {
void install();
} // namespace ops
