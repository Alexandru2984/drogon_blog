#pragma once

#include <cstdint>
#include <string>

namespace metrics {

// Records that a request finished. `routePattern` should be the Drogon-matched
// path pattern (e.g. "/posts/{1}") to keep label cardinality bounded.
void observeRequest(const std::string& routePattern,
                    const std::string& method,
                    int                status,
                    double             latencySeconds);

// In-flight request gauge — paired calls from the access log around request
// processing. Stays correct under concurrency because both ends are atomic.
void incInFlight();
void decInFlight();

// Renders the current process-wide metric values in the Prometheus text
// exposition format. Safe to call concurrently with observeRequest().
std::string renderPrometheus();

} // namespace metrics
