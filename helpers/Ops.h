#pragma once

// Registers operational HTTP routes:
//   GET /healthz  → liveness, always 200 if the process is alive
//   GET /readyz   → readiness, checks a DB round-trip; returns 503 once
//                   `beginDrain()` has been called so an upstream load
//                   balancer pulls us out of the pool before SIGTERM hits.
//   GET /metrics  → Prometheus text exposition; gated by METRICS_TOKEN env var
//                   (Bearer auth) when set, otherwise only loopback peers are
//                   accepted.
namespace ops {

void install();

// Flip the drain flag. Idempotent. After this call, /readyz returns 503
// (status: "draining"). /healthz keeps returning 200 — we are alive, just
// refusing new traffic. The signal handler in main.cc calls this on
// SIGTERM/SIGINT before invoking app().quit().
void beginDrain();

// Whether beginDrain() has been called. Other subsystems (the WS hub,
// the rate limiter) can short-circuit on drain instead of taking new
// work that the connection will be killed mid-way through.
bool isDraining();

} // namespace ops
