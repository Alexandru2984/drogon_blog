#pragma once

// Installs request-ID propagation, per-request structured JSON access logs,
// and request-latency metrics ingestion. Idempotent — call once after
// loadConfigJson and before run().
namespace access_log {
void install();
} // namespace access_log
