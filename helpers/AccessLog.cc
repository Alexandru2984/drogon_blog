#include "AccessLog.h"
#include "LogSafety.h"
#include "Metrics.h"
#include "Security.h"
#include "Sentry.h"
#include "Tracing.h"

#include <drogon/drogon.h>

#include <chrono>
#include <cstdio>
#include <string>
#include <unistd.h>
#include <unordered_set>

namespace access_log {

namespace {

constexpr const char* kReqIdAttr   = "blog.req_id";
constexpr const char* kStartAttr   = "blog.t_start";
constexpr const char* kReqIdHeader = "X-Request-Id";

// Endpoints we don't want flooding the access log or the metrics histogram
// (probes and scrape targets hit them constantly).
const std::unordered_set<std::string>& mutedPaths()
{
    static const std::unordered_set<std::string> s = {
        "/healthz", "/readyz", "/metrics",
    };
    return s;
}

const char* methodName(drogon::HttpMethod m)
{
    switch (m) {
        case drogon::Get:     return "GET";
        case drogon::Post:    return "POST";
        case drogon::Put:     return "PUT";
        case drogon::Delete:  return "DELETE";
        case drogon::Head:    return "HEAD";
        case drogon::Options: return "OPTIONS";
        case drogon::Patch:   return "PATCH";
        default:              return "?";
    }
}

} // namespace

void install()
{
    using namespace drogon;

    // Stamp every incoming request: generate (or honour) a request ID, start
    // the W3C trace context, increment the in-flight gauge, and record the
    // start time so the post-handling hook can compute latency.
    app().registerPreRoutingAdvice(
        [](const HttpRequestPtr& req) {
            // Echo a client-supplied request ID iff it's safe to put back
            // on the wire: bounded length and printable ASCII only. A
            // raw header value would be a response-header injection
            // vector (CR/LF → response splitting) and a log-injection
            // vector (the access log line would absorb whatever bytes
            // came in). Drop and regenerate when either check fails.
            constexpr std::size_t kMaxReqId = 64;
            auto incoming = req->getHeader(kReqIdHeader);
            std::string id;
            if (!incoming.empty() && incoming.size() <= kMaxReqId) {
                bool ok = true;
                for (unsigned char c : incoming) {
                    if (c < 0x21 || c > 0x7E) { ok = false; break; }
                }
                if (ok) id = incoming;
            }
            if (id.empty()) id = security::randomToken(12);
            req->attributes()->insert(kReqIdAttr, id);
            req->attributes()->insert(
                kStartAttr, std::chrono::steady_clock::now());
            tracing::startServerSpan(req);
            metrics::incInFlight();
        });

    app().registerPostHandlingAdvice(
        [](const HttpRequestPtr& req, const HttpResponsePtr& resp) {
            const auto attrs = req->attributes();
            std::string id;
            if (attrs->find(kReqIdAttr)) id = attrs->get<std::string>(kReqIdAttr);

            // Echo the ID back so callers (and downstream services) can
            // correlate logs.
            if (!id.empty()) resp->addHeader(kReqIdHeader, id);

            std::chrono::steady_clock::time_point start{};
            if (attrs->find(kStartAttr))
                start = attrs->get<std::chrono::steady_clock::time_point>(kStartAttr);

            const auto elapsed = std::chrono::steady_clock::now() - start;
            const double latencySec =
                std::chrono::duration<double>(elapsed).count();
            const double latencyMs = latencySec * 1000.0;

            // Close the trace span (emits OTLP-shaped JSON to stderr when
            // BLOG_TRACE_LOG=1; always sets the traceparent response header).
            // Done unconditionally so even muted paths propagate context.
            tracing::finishServerSpan(req, resp, latencySec);
            metrics::decInFlight();

            const std::string path = req->getPath();
            if (mutedPaths().count(path)) return;

            const std::string method = methodName(req->getMethod());
            const std::string ip     = security::clientIp(
                std::const_pointer_cast<HttpRequest>(req));
            const int  status        = static_cast<int>(resp->getStatusCode());
            const auto bytes         = resp->getBody().size();

            // Prefer the matched route pattern so metrics labels don't
            // explode on path parameters. Every unmatched URL shares one
            // sentinel; the raw path remains available in the bounded log.
            std::string route{req->getMatchedPathPattern()};
            if (route.empty()) route = "/__unmatched__";

            metrics::observeRequest(route, method, status, latencySec);

            // Trace correlation: a log line plus the matching span share the
            // same trace_id, which is what makes "click a slow request → see
            // its span tree" work in Grafana / Tempo.
            const std::string traceId = tracing::traceIdOf(req);
            const std::string spanId  = tracing::spanIdOf(req);

            const auto safeId    = log_safety::escapeJsonField(id, 128);
            const auto safePath  = log_safety::escapeJsonField(path, 512);
            const auto safeRoute = log_safety::escapeJsonField(route, 512);
            const auto safeIp    = log_safety::escapeJsonField(ip, 128);
            if (safeId.abbreviated || safePath.abbreviated ||
                safeRoute.abbreviated || safeIp.abbreviated)
            {
                metrics::noteObservabilityInputTruncated();
            }

            char latency[64];
            const int latencyLength =
                std::snprintf(latency, sizeof(latency), "%.3f", latencyMs);
            const std::string safeLatency =
                latencyLength > 0 &&
                static_cast<std::size_t>(latencyLength) < sizeof(latency)
                    ? std::string(latency,
                                  static_cast<std::size_t>(latencyLength))
                    : std::string("0.000");

            std::string line;
            line.reserve(512 + safePath.text.size() + safeRoute.text.size());
            line += "{\"ts\":\"";
            line += log_safety::isoUtcNow();
            line += "\",\"req_id\":\"";
            line += safeId.text;
            line += "\",\"trace_id\":\"";
            line += traceId;
            line += "\",\"span_id\":\"";
            line += spanId;
            line += "\",\"method\":\"";
            line += method;
            line += "\",\"path\":\"";
            line += safePath.text;
            line += "\",\"route\":\"";
            line += safeRoute.text;
            line += "\",\"status\":";
            line += std::to_string(status);
            line += ",\"latency_ms\":";
            line += safeLatency;
            line += ",\"bytes\":";
            line += std::to_string(bytes);
            line += ",\"ip\":\"";
            line += safeIp.text;
            line += "\"}\n";

            // One write() syscall per line. journald reads STDOUT_FILENO as a
            // stream socket; writes ≤ PIPE_BUF (4096 B) — our lines stay
            // below 2 KB — land atomically in the kernel buffer, so we
            // don't need to bracket with a mutex any more. fwrite/fflush
            // were the previous shape: two libc indirections + a separate
            // flush syscall per request, plus a global lock that serialised
            // every IO thread on the access path.
            // Return value intentionally unused: if journald disappears we
            // drop the line rather than block the request path. Every
            // attacker-controlled field has an encoded budget above, keeping
            // this below PIPE_BUF and making the single write atomic.
            [[maybe_unused]] const auto written =
                ::write(STDOUT_FILENO, line.data(), line.size());

            // Escalate server errors to Sentry. 4xx is client-driven
            // and would drown the dashboard; 5xx is on us. fire-and-
            // forget — sentry::captureRequestError is a no-op when
            // BLOG_SENTRY_DSN is unset.
            if (status >= 500) {
                sentry::captureRequestError(
                    req, resp, sentry::Level::Error,
                    method + " " + route + " → " + std::to_string(status));
            }
        });
}

} // namespace access_log
