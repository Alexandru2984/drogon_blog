#include "AccessLog.h"
#include "Metrics.h"
#include "Security.h"
#include "Tracing.h"

#include <drogon/drogon.h>

#include <chrono>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <string>
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

std::string nowIsoUtc()
{
    using namespace std::chrono;
    const auto now  = system_clock::now();
    const auto secs = time_point_cast<seconds>(now);
    const auto ms   = duration_cast<milliseconds>(now - secs).count();

    std::time_t t = system_clock::to_time_t(secs);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%03ldZ",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec, static_cast<long>(ms));
    return buf;
}

std::string jsonEscape(const std::string& s)
{
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out.push_back(c);
                }
        }
    }
    return out;
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

std::mutex g_stdoutMu; // keep lines atomic across threads

} // namespace

void install()
{
    using namespace drogon;

    // Stamp every incoming request: generate (or honour) a request ID, start
    // the W3C trace context, increment the in-flight gauge, and record the
    // start time so the post-handling hook can compute latency.
    app().registerPreRoutingAdvice(
        [](const HttpRequestPtr& req) {
            auto incoming = req->getHeader(kReqIdHeader);
            std::string id = !incoming.empty() ? incoming : security::randomToken(12);
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

            // Prefer the matched route pattern so metrics labels don't explode
            // on path parameters; fall back to the raw path otherwise.
            std::string route{req->getMatchedPathPattern()};
            if (route.empty()) route = path;

            metrics::observeRequest(route, method, status, latencySec);

            // Trace correlation: a log line plus the matching span share the
            // same trace_id, which is what makes "click a slow request → see
            // its span tree" work in Grafana / Tempo.
            const std::string traceId = tracing::traceIdOf(req);
            const std::string spanId  = tracing::spanIdOf(req);

            char line[1280];
            int n = std::snprintf(
                line, sizeof(line),
                "{\"ts\":\"%s\",\"req_id\":\"%s\",\"trace_id\":\"%s\",\"span_id\":\"%s\","
                "\"method\":\"%s\",\"path\":\"%s\",\"route\":\"%s\",\"status\":%d,"
                "\"latency_ms\":%.3f,\"bytes\":%zu,\"ip\":\"%s\"}\n",
                nowIsoUtc().c_str(),
                jsonEscape(id).c_str(),
                traceId.c_str(),
                spanId.c_str(),
                method.c_str(),
                jsonEscape(path).c_str(),
                jsonEscape(route).c_str(),
                status,
                latencyMs,
                bytes,
                jsonEscape(ip).c_str());

            std::lock_guard<std::mutex> lk(g_stdoutMu);
            std::fwrite(line, 1, n > 0 ? static_cast<std::size_t>(n) : 0, stdout);
            std::fflush(stdout);
        });
}

} // namespace access_log
