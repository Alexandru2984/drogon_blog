#pragma once

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>

#include <string>

// Lightweight W3C Trace Context propagation + OTLP-shaped span emission.
//
// We deliberately do NOT depend on opentelemetry-cpp here: the full SDK pulls
// in protobuf + gRPC + a fairly heavy build graph for a service that already
// emits a structured JSON access log. Instead, we:
//
//   1. Parse incoming `traceparent` headers and propagate them on the response
//      so downstream agents can stitch traces.
//   2. Generate fresh trace_id / span_id (libsodium randombytes) when none are
//      present, applying head-based sampling (BLOG_TRACE_SAMPLE_RATE, default 1.0).
//   3. Emit one JSON line per request to stderr in a shape that matches the
//      OTLP/HTTP-JSON `Span` representation closely enough that a collector
//      like Vector or fluent-bit can convert it to real OTLP and forward to
//      Tempo / Jaeger / Honeycomb without code changes.
//
// Output is gated by BLOG_TRACE_LOG=1 (default off; the access log already
// covers the common observability needs).
namespace tracing {

struct Context {
    std::string trace_id;   // 32 hex chars (16 bytes)
    std::string span_id;    // 16 hex chars (8 bytes)
    std::string parent_id;  // 16 hex chars, empty when this is the root span
    bool        sampled = true;
};

// Reads `traceparent` from the incoming request (or generates a fresh context
// when absent) and stamps it onto req->attributes(). Returns the context that
// should be propagated through any child operations spawned from this request.
Context startServerSpan(const drogon::HttpRequestPtr& req);

// Emits the OTLP-shaped span JSON to stderr (when BLOG_TRACE_LOG=1) and writes
// the W3C `traceparent` header onto the response so external callers / agents
// can correlate. `durationSeconds` is the wall-clock latency measured by the
// access log.
void finishServerSpan(const drogon::HttpRequestPtr& req,
                      const drogon::HttpResponsePtr& resp,
                      double durationSeconds);

// Accessors so other layers (access log, structured error reporting, …) can
// pick up the active IDs without re-parsing the header. Both return empty
// strings when no context was attached.
std::string traceIdOf(const drogon::HttpRequestPtr& req);
std::string spanIdOf(const drogon::HttpRequestPtr& req);

} // namespace tracing
