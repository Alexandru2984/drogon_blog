#include "Sentry.h"

#include "Security.h"        // for randomToken / similar; we use libsodium directly
#include "Tracing.h"

#include <drogon/HttpClient.h>
#include <drogon/drogon.h>
#include <trantor/utils/Logger.h>

#include <sodium.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>

namespace sentry {

namespace {

// Parsed DSN — populated once at install().
struct DsnState {
    std::string baseUrl;     // "https://o123.ingest.sentry.io" (no trailing slash)
    std::string projectId;   // "456"
    std::string publicKey;   // "abcdef…"
};

DsnState                                 g_dsn;
drogon::HttpClientPtr                    g_client;     // bound to DSN host
std::atomic<bool>                        g_armed{false};
std::mutex                               g_mu;        // protects construction only

// Parse a Sentry DSN of the shape:
//   https://<public_key>@<host>[:port]/<project_id>
// Returns valid=false on any deviation. We don't accept the optional
// `secret_key` (deprecated by Sentry years ago).
bool parseDsn(const std::string& s, DsnState& out)
{
    constexpr std::string_view https = "https://";
    constexpr std::string_view http  = "http://";
    bool isHttps = s.rfind(https, 0) == 0;
    bool isHttp  = !isHttps && (s.rfind(http, 0) == 0);
    if (!isHttps && !isHttp) return false;

    const std::size_t prefixLen = isHttps ? https.size() : http.size();
    const auto at = s.find('@', prefixLen);
    if (at == std::string::npos) return false;
    out.publicKey = s.substr(prefixLen, at - prefixLen);
    if (out.publicKey.empty()) return false;

    const auto slash = s.find('/', at + 1);
    if (slash == std::string::npos) return false;
    const std::string host = s.substr(at + 1, slash - at - 1);
    if (host.empty()) return false;

    const std::string project = s.substr(slash + 1);
    if (project.empty()) return false;

    out.baseUrl   = std::string(isHttps ? "https://" : "http://") + host;
    out.projectId = project;
    // Strip any trailing slash defensively.
    if (!out.projectId.empty() && out.projectId.back() == '/') out.projectId.pop_back();
    return true;
}

// UUID v4 string without dashes (32 hex chars). Sentry accepts that form.
// libsodium is already linked everywhere we use auth tokens; this just
// makes the same RNG drive the event_id field.
std::string uuid4NoDashes()
{
    unsigned char buf[16];
    randombytes_buf(buf, sizeof(buf));
    // RFC 4122 v4 markers: clear/set the version + variant bits.
    buf[6] = (buf[6] & 0x0F) | 0x40;
    buf[8] = (buf[8] & 0x3F) | 0x80;
    static const char* hex = "0123456789abcdef";
    std::string out;
    out.reserve(32);
    for (int i = 0; i < 16; ++i) {
        out.push_back(hex[(buf[i] >> 4) & 0xF]);
        out.push_back(hex[buf[i] & 0xF]);
    }
    return out;
}

std::string isoUtcNow()
{
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  now.time_since_epoch()) % 1000;
    std::tm tm{};
    gmtime_r(&t, &tm);
    // 64, not 40. The format is 24 characters for any real timestamp, but
    // the compiler reasons about the declared widths rather than the
    // values: %04d on an int can print 11 characters, and it warned that
    // the output could reach 78 bytes. snprintf would truncate rather than
    // overflow, so this was never a memory-safety bug — but a truncated
    // timestamp silently corrupts the event Sentry receives, and the
    // warning is worth removing rather than living with.
    char buf[64];
    std::snprintf(buf, sizeof(buf),
        "%04d-%02d-%02dT%02d:%02d:%02d.%03lldZ",
        tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
        tm.tm_hour, tm.tm_min, tm.tm_sec,
        static_cast<long long>(ms.count()));
    return buf;
}

const char* levelName(Level lvl)
{
    switch (lvl) {
        case Level::Warning: return "warning";
        case Level::Error:   return "error";
        case Level::Fatal:   return "fatal";
    }
    return "error";
}

// JSON-escape minimal — only the bytes that break a JSON string. The
// access log helper has a fuller implementation; we keep this scoped
// to the strings we feed Sentry (paths, method, ids, message). They
// never contain control bytes beyond \n in pathological cases.
std::string jsonEscape(std::string_view s)
{
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"':  out += "\\\""; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    return out;
}

} // namespace

bool install()
{
    const char* env = std::getenv("BLOG_SENTRY_DSN");
    if (!env || !*env) {
        LOG_INFO << "sentry: BLOG_SENTRY_DSN unset; disabled.";
        return false;
    }
    DsnState s;
    if (!parseDsn(env, s)) {
        LOG_ERROR << "sentry: malformed BLOG_SENTRY_DSN (want "
                  << "https://<public_key>@<host>/<project_id>).";
        return false;
    }
    std::lock_guard<std::mutex> lk(g_mu);
    g_dsn = std::move(s);
    g_client = drogon::HttpClient::newHttpClient(g_dsn.baseUrl);
    if (!g_client) {
        LOG_ERROR << "sentry: HttpClient construction failed for " << g_dsn.baseUrl;
        return false;
    }
    g_armed.store(true, std::memory_order_release);
    LOG_INFO << "sentry: armed for " << g_dsn.baseUrl
             << " project=" << g_dsn.projectId;
    return true;
}

void stop()
{
    g_armed.store(false, std::memory_order_release);
    std::lock_guard<std::mutex> lk(g_mu);
    g_client.reset();
}

void captureRequestError(const drogon::HttpRequestPtr&  req,
                         const drogon::HttpResponsePtr& resp,
                         Level                          level,
                         const std::string&             message)
{
    if (!g_armed.load(std::memory_order_acquire)) return;

    const std::string eventId = uuid4NoDashes();
    const std::string ts      = isoUtcNow();

    const std::string route  = std::string(req->getMatchedPathPattern().empty()
        ? req->getPath() : req->getMatchedPathPattern());
    const std::string method = req->getMethodString();
    const int         status = static_cast<int>(resp->getStatusCode());

    // X-Request-Id from the access log advice (echoed back to clients).
    const std::string reqId = resp->getHeader("X-Request-Id");
    const std::string ua    = req->getHeader("User-Agent");
    const std::string url   = req->getPath();
    const std::string trace = tracing::traceIdOf(req);

    // Build the event payload by hand. We avoid Json::Value here so a
    // failure in Sentry's parser doesn't perturb the request path; a
    // string buffer is also easier to inspect in a packet capture.
    std::string body;
    body.reserve(1024);
    body += "{\"event_id\":\"";          body += eventId;            body += "\",";
    body += "\"timestamp\":\"";          body += ts;                 body += "\",";
    body += "\"platform\":\"other\",";
    body += "\"logger\":\"blog\",";
    body += "\"level\":\"";              body += levelName(level);   body += "\",";
    body += "\"message\":{\"formatted\":\""; body += jsonEscape(message); body += "\"},";
    body += "\"tags\":{";
    body += "\"route\":\"";              body += jsonEscape(route);  body += "\",";
    body += "\"method\":\"";             body += jsonEscape(method); body += "\",";
    body += "\"status\":\"";             body += std::to_string(status); body += "\",";
    body += "\"request_id\":\"";         body += jsonEscape(reqId);  body += "\"";
    body += "},";
    body += "\"request\":{";
    body += "\"url\":\"";                body += jsonEscape(url);    body += "\",";
    body += "\"method\":\"";             body += jsonEscape(method); body += "\",";
    body += "\"headers\":{";
    if (!ua.empty()) {
        body += "\"User-Agent\":\"";    body += jsonEscape(ua);     body += "\"";
    }
    body += "}";
    body += "},";
    body += "\"extra\":{\"trace_id\":\""; body += jsonEscape(trace); body += "\"}";
    body += "}";

    // X-Sentry-Auth header — the only auth Sentry needs for the
    // public ingest endpoint. sentry_timestamp is the client clock,
    // accepted as advisory by Sentry/Glitchtip.
    char authBuf[256];
    std::snprintf(authBuf, sizeof(authBuf),
        "Sentry sentry_version=7, sentry_client=blog/1.0, "
        "sentry_key=%s, sentry_timestamp=%lld",
        g_dsn.publicKey.c_str(),
        static_cast<long long>(std::time(nullptr)));

    const std::string path = "/api/" + g_dsn.projectId + "/store/";

    auto post = drogon::HttpRequest::newHttpRequest();
    post->setMethod(drogon::Post);
    post->setPath(path);
    post->setContentTypeString("application/json");
    post->addHeader("X-Sentry-Auth", authBuf);
    post->setBody(std::move(body));

    // Fire-and-forget. Capture a copy of the client to keep it alive
    // until the callback runs (g_client might be reset on stop()).
    drogon::HttpClientPtr client;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        client = g_client;
    }
    if (!client) return;
    client->sendRequest(
        post,
        [eventId](drogon::ReqResult result, const drogon::HttpResponsePtr& r) {
            if (result != drogon::ReqResult::Ok || !r) {
                LOG_WARN << "sentry: capture send failed (eid=" << eventId
                         << ", result=" << static_cast<int>(result) << ")";
                return;
            }
            const int code = static_cast<int>(r->getStatusCode());
            if (code >= 200 && code < 300) {
                LOG_DEBUG << "sentry: event " << eventId << " accepted ("
                          << code << ")";
            } else {
                LOG_WARN << "sentry: event " << eventId << " rejected ("
                         << code << "): " << r->getBody();
            }
        },
        /*timeout=*/5.0);
}

} // namespace sentry
