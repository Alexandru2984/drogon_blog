#include "Tracing.h"

#include <sodium.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>

namespace tracing {

namespace {

constexpr const char* kCtxAttr      = "blog.trace_ctx";
constexpr const char* kStartNsAttr  = "blog.trace_start_ns";
constexpr const char* kTraceHeader  = "traceparent";

const std::string& serviceName()
{
    static const std::string s = [] {
        const char* v = std::getenv("BLOG_SERVICE_NAME");
        return (v && *v) ? std::string(v) : std::string("blog");
    }();
    return s;
}

bool traceLogEnabled()
{
    static const bool b = [] {
        const char* v = std::getenv("BLOG_TRACE_LOG");
        return v && std::string(v) == "1";
    }();
    return b;
}

double sampleRate()
{
    static const double r = [] {
        const char* v = std::getenv("BLOG_TRACE_SAMPLE_RATE");
        if (!v || !*v) return 1.0;
        try { return std::clamp(std::stod(v), 0.0, 1.0); }
        catch (...) { return 1.0; }
    }();
    return r;
}

std::string hexEncode(const unsigned char* data, std::size_t n)
{
    static const char kHex[] = "0123456789abcdef";
    std::string out(n * 2, '\0');
    for (std::size_t i = 0; i < n; ++i) {
        out[i * 2]     = kHex[(data[i] >> 4) & 0xF];
        out[i * 2 + 1] = kHex[data[i] & 0xF];
    }
    return out;
}

bool isHex(const std::string& s)
{
    return std::all_of(s.begin(), s.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    });
}

bool isAllZero(const std::string& hex)
{
    return std::all_of(hex.begin(), hex.end(), [](char c) { return c == '0'; });
}

std::string randomHex(std::size_t bytes)
{
    unsigned char buf[16];
    if (bytes > sizeof(buf)) bytes = sizeof(buf);
    randombytes_buf(buf, bytes);
    return hexEncode(buf, bytes);
}

bool decideSampled(const std::string& trace_id, bool inheritedSampled)
{
    if (sampleRate() >= 1.0) return true;
    if (sampleRate() <= 0.0) return false;
    if (inheritedSampled)    return true; // honour parent's decision
    // Deterministic: hash trace_id's first 8 hex chars (32 bits) and compare.
    unsigned long bits = std::strtoul(trace_id.substr(0, 8).c_str(), nullptr, 16);
    return (static_cast<double>(bits) / 4294967295.0) < sampleRate();
}

// Parse W3C traceparent: `00-<32hex trace>-<16hex span>-<2hex flags>`.
// Anything malformed is treated as "no context"; we'll mint a fresh one.
bool parseTraceparent(const std::string& header, Context& out)
{
    if (header.size() != 55) return false;
    if (header[2] != '-' || header[35] != '-' || header[52] != '-') return false;
    const std::string version = header.substr(0, 2);
    const std::string trace   = header.substr(3, 32);
    const std::string span    = header.substr(36, 16);
    const std::string flags   = header.substr(53, 2);
    if (version == "ff") return false;
    if (!isHex(version) || !isHex(trace) || !isHex(span) || !isHex(flags)) return false;
    if (isAllZero(trace) || isAllZero(span)) return false;

    out.trace_id  = trace;
    out.parent_id = span;
    out.sampled   = (std::strtoul(flags.c_str(), nullptr, 16) & 0x01) != 0;
    return true;
}

std::string formatTraceparent(const Context& c)
{
    char flags[3];
    std::snprintf(flags, sizeof(flags), "%02x", c.sampled ? 0x01 : 0x00);
    return std::string("00-") + c.trace_id + "-" + c.span_id + "-" + flags;
}

// Monotonic clock for duration, wall clock for timestamps. Combined gives
// stable spans even when the system clock jumps.
std::uint64_t monotonicNanos()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::uint64_t wallNanos()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
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

std::mutex g_stderrMu;

} // namespace

Context startServerSpan(const drogon::HttpRequestPtr& req)
{
    Context ctx;
    const auto& incoming = req->getHeader(kTraceHeader);
    if (!incoming.empty() && parseTraceparent(incoming, ctx)) {
        // Continue an existing trace: keep their trace_id + parent_id, mint a
        // new span_id for our work.
        ctx.span_id = randomHex(8);
        ctx.sampled = decideSampled(ctx.trace_id, ctx.sampled);
    } else {
        ctx.trace_id  = randomHex(16);
        ctx.span_id   = randomHex(8);
        ctx.parent_id.clear();
        ctx.sampled   = decideSampled(ctx.trace_id, false);
    }

    auto attrs = req->attributes();
    attrs->insert(kCtxAttr,     ctx);
    attrs->insert(kStartNsAttr, monotonicNanos());
    return ctx;
}

void finishServerSpan(const drogon::HttpRequestPtr&  req,
                      const drogon::HttpResponsePtr& resp,
                      double durationSeconds)
{
    auto attrs = req->attributes();
    if (!attrs->find(kCtxAttr)) return;
    const auto ctx = attrs->get<Context>(kCtxAttr);

    // Always propagate to the client so they can correlate even when we don't
    // sample. This is a pure header — no PII, ~55 bytes.
    resp->addHeader("traceparent", formatTraceparent(ctx));

    if (!ctx.sampled || !traceLogEnabled()) return;

    const std::uint64_t endNanos   = wallNanos();
    const std::uint64_t startNanos =
        endNanos - static_cast<std::uint64_t>(durationSeconds * 1e9);

    std::string route{req->getMatchedPathPattern()};
    if (route.empty()) route = req->getPath();
    const char* method = methodName(req->getMethod());
    const int   status = static_cast<int>(resp->getStatusCode());

    // OTLP/HTTP-JSON span shape: a single `spans[]` entry with kind=SERVER (2).
    // Keep it on one line — collectors expect newline-delimited JSON for log
    // ingestion, and our access log already follows that convention.
    char buf[1024];
    int n = std::snprintf(
        buf, sizeof(buf),
        "{\"kind\":\"otlp_span\","
        "\"resource\":{\"service.name\":\"%s\"},"
        "\"trace_id\":\"%s\","
        "\"span_id\":\"%s\","
        "\"parent_span_id\":\"%s\","
        "\"name\":\"HTTP %s %s\","
        "\"span_kind\":\"SERVER\","
        "\"start_time_unix_nano\":%llu,"
        "\"end_time_unix_nano\":%llu,"
        "\"attributes\":{"
            "\"http.request.method\":\"%s\","
            "\"http.route\":\"%s\","
            "\"http.response.status_code\":%d,"
            "\"http.response.body.size\":%zu"
        "},"
        "\"status\":{\"code\":%d}}\n",
        jsonEscape(serviceName()).c_str(),
        ctx.trace_id.c_str(),
        ctx.span_id.c_str(),
        ctx.parent_id.c_str(),
        method, jsonEscape(route).c_str(),
        static_cast<unsigned long long>(startNanos),
        static_cast<unsigned long long>(endNanos),
        method,
        jsonEscape(route).c_str(),
        status,
        resp->getBody().size(),
        status >= 500 ? 2 /* ERROR */ : 1 /* OK */);

    if (n <= 0) return;
    std::lock_guard<std::mutex> lk(g_stderrMu);
    std::fwrite(buf, 1, static_cast<std::size_t>(n), stderr);
    std::fflush(stderr);
}

std::string traceIdOf(const drogon::HttpRequestPtr& req)
{
    auto attrs = req->attributes();
    if (!attrs->find(kCtxAttr)) return {};
    return attrs->get<Context>(kCtxAttr).trace_id;
}

std::string spanIdOf(const drogon::HttpRequestPtr& req)
{
    auto attrs = req->attributes();
    if (!attrs->find(kCtxAttr)) return {};
    return attrs->get<Context>(kCtxAttr).span_id;
}

} // namespace tracing
