#include "Security.h"

#include <drogon/drogon.h>
#include <sodium.h>

#include <chrono>
#include <cstdlib>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace security {

namespace {

bool isTrustedProxy(const std::string& ip)
{
    // nginx reverse-proxies us on loopback; Docker / VPN ranges may also be
    // legitimate. Anything beyond these should not be allowed to spoof XFF.
    if (ip.rfind("127.", 0) == 0) return true;
    if (ip == "::1")              return true;
    if (ip.rfind("10.", 0) == 0)  return true;
    if (ip.rfind("172.", 0) == 0) return true;
    if (ip.rfind("192.168.", 0) == 0) return true;
    return false;
}

std::string firstHop(const std::string& xff)
{
    auto comma = xff.find(',');
    std::string first = (comma == std::string::npos) ? xff : xff.substr(0, comma);
    while (!first.empty() && std::isspace(static_cast<unsigned char>(first.front()))) first.erase(first.begin());
    while (!first.empty() && std::isspace(static_cast<unsigned char>(first.back())))  first.pop_back();
    return first;
}

const std::string kCsrfCookieName = "csrf_token";

struct Bucket {
    double tokens;
    std::chrono::steady_clock::time_point lastRefill;
};

std::mutex                              g_mu;
std::unordered_map<std::string, Bucket> g_buckets;

} // namespace

std::string clientIp(const drogon::HttpRequestPtr& req)
{
    const std::string peer = req->getPeerAddr().toIp();

    // Cloudflare wins outright in production.
    const auto& cf = req->getHeader("CF-Connecting-IP");
    if (!cf.empty() && isTrustedProxy(peer)) return cf;

    const auto& xff = req->getHeader("X-Forwarded-For");
    if (!xff.empty() && isTrustedProxy(peer)) return firstHop(xff);

    return peer;
}

std::string randomToken(std::size_t bytes)
{
    std::string raw(bytes, '\0');
    randombytes_buf(raw.data(), raw.size());

    // URL-safe base64 without padding. Keeps cookies/headers clean.
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    out.reserve((bytes * 4 + 2) / 3);
    std::size_t i = 0;
    while (i + 3 <= bytes) {
        unsigned a = static_cast<unsigned char>(raw[i]);
        unsigned b = static_cast<unsigned char>(raw[i + 1]);
        unsigned c = static_cast<unsigned char>(raw[i + 2]);
        out.push_back(alphabet[(a >> 2) & 0x3F]);
        out.push_back(alphabet[((a & 0x3) << 4) | ((b >> 4) & 0xF)]);
        out.push_back(alphabet[((b & 0xF) << 2) | ((c >> 6) & 0x3)]);
        out.push_back(alphabet[c & 0x3F]);
        i += 3;
    }
    if (i < bytes) {
        unsigned a = static_cast<unsigned char>(raw[i]);
        unsigned b = (i + 1 < bytes) ? static_cast<unsigned char>(raw[i + 1]) : 0;
        out.push_back(alphabet[(a >> 2) & 0x3F]);
        out.push_back(alphabet[((a & 0x3) << 4) | ((b >> 4) & 0xF)]);
        if (i + 1 < bytes)
            out.push_back(alphabet[(b & 0xF) << 2]);
    }
    return out;
}

const std::string& csrfCookieName() { return kCsrfCookieName; }

RateLimitDecision rateLimitTake(const std::string& bucketName,
                                const std::string& key,
                                double capacity,
                                double refillPerSecond)
{
    const std::string composite = bucketName + "|" + key;
    auto now = std::chrono::steady_clock::now();

    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_buckets.find(composite);
    if (it == g_buckets.end()) {
        g_buckets.emplace(composite, Bucket{capacity - 1.0, now});
        return {true, 0.0};
    }

    Bucket& b = it->second;
    double elapsed = std::chrono::duration<double>(now - b.lastRefill).count();
    b.tokens = std::min(capacity, b.tokens + elapsed * refillPerSecond);
    b.lastRefill = now;

    if (b.tokens >= 1.0) {
        b.tokens -= 1.0;
        return {true, 0.0};
    }
    return {false, (1.0 - b.tokens) / refillPerSecond};
}

void registerAdvices()
{
    // Per-IP token buckets for sensitive auth endpoints. Capacity is the burst
    // budget; refill rate sets the long-run cap. Tuned to be friendly to humans
    // (5 logins within a few seconds is fine) while killing credential stuffing.
    struct RateRule { const char* path; double capacity; double refillPerSec; };
    static const RateRule kRateRules[] = {
        {"/auth/login",                5.0, 5.0  / 60.0},   // 5 burst, 5 / min
        {"/auth/register",             3.0, 3.0  / 600.0},  // 3 burst, 3 / 10 min
        {"/auth/request-reset",        3.0, 3.0  / 600.0},
        {"/auth/resend-verification",  3.0, 3.0  / 600.0},
        {"/auth/reset-password",       5.0, 5.0  / 60.0},
    };

    static const std::unordered_set<std::string> kCsrfExempt = {
        "/auth/login", "/auth/register",
        "/auth/request-reset", "/auth/reset-password",
        "/auth/verify-email", "/auth/resend-verification",
    };

    const char* disableRl = std::getenv("BLOG_DISABLE_RATE_LIMIT");
    const bool rateLimitEnabled = !(disableRl && std::string(disableRl) == "1");

    drogon::app().registerSyncAdvice(
        [rateLimitEnabled](const drogon::HttpRequestPtr& req) -> drogon::HttpResponsePtr {
            const auto path   = req->getPath();
            const auto method = req->getMethod();

            // ---- Rate limit ----
            if (rateLimitEnabled && method == drogon::Post) {
                for (const auto& rule : kRateRules) {
                    if (path == rule.path) {
                        auto d = rateLimitTake(
                            rule.path, clientIp(req),
                            rule.capacity, rule.refillPerSec);
                        if (!d.allowed) {
                            Json::Value body;
                            body["error"] = "Too many requests";
                            auto resp = drogon::HttpResponse::newHttpJsonResponse(body);
                            resp->setStatusCode(drogon::k429TooManyRequests);
                            resp->addHeader("Retry-After",
                                std::to_string(static_cast<int>(d.retryAfterSeconds) + 1));
                            return resp;
                        }
                        break;
                    }
                }
            }

            // ---- CSRF (double-submit cookie) ----
            // Safe methods do not mutate state; pre-auth endpoints can't have a
            // CSRF cookie yet so they're exempt by path.
            if (method != drogon::Get && method != drogon::Head &&
                method != drogon::Options && !kCsrfExempt.count(path))
            {
                const auto cookieTok = req->getCookie(csrfCookieName());
                const auto headerTok = req->getHeader("X-CSRF-Token");
                if (cookieTok.empty() || cookieTok != headerTok) {
                    Json::Value body;
                    body["error"] = "Invalid or missing CSRF token";
                    auto resp = drogon::HttpResponse::newHttpJsonResponse(body);
                    resp->setStatusCode(drogon::k403Forbidden);
                    return resp;
                }
            }

            return nullptr; // continue to handler
        });

    drogon::app().registerPostHandlingAdvice(
        [](const drogon::HttpRequestPtr&, const drogon::HttpResponsePtr& resp) {
            applySecurityHeaders(resp);
        });

    // Drogon attaches the session cookie *after* PostHandling, so we patch it
    // at the very last step before serialization. When BLOG_SECURE_COOKIES=1
    // the JSESSIONID cookie is re-emitted with the Secure flag.
    drogon::app().registerPreSendingAdvice(
        [](const drogon::HttpRequestPtr&, const drogon::HttpResponsePtr& resp) {
            if (!secureCookies()) return;
            const auto& jar = resp->getCookies();
            auto it = jar.find("JSESSIONID");
            if (it == jar.end() || it->second.value().empty()) return;
            if (it->second.isSecure()) return;          // already done
            drogon::Cookie c = it->second;
            c.setSecure(true);
            resp->removeCookie("JSESSIONID");
            resp->addCookie(std::move(c));
        });
}

bool secureCookies()
{
    const char* v = std::getenv("BLOG_SECURE_COOKIES");
    return v && std::string(v) == "1";
}

std::string hashPassword(const std::string& password)
{
    char out[crypto_pwhash_STRBYTES];
    if (crypto_pwhash_str(out,
                          password.c_str(),
                          password.size(),
                          crypto_pwhash_OPSLIMIT_INTERACTIVE,
                          crypto_pwhash_MEMLIMIT_INTERACTIVE) != 0)
    {
        throw std::runtime_error("password hashing failed");
    }
    return std::string(out);
}

bool verifyPassword(const std::string& storedHash, const std::string& candidate)
{
    return crypto_pwhash_str_verify(storedHash.c_str(),
                                    candidate.c_str(),
                                    candidate.size()) == 0;
}

void applySecurityHeaders(const drogon::HttpResponsePtr& resp)
{
    resp->addHeader("Strict-Transport-Security", "max-age=31536000; includeSubDomains");
    resp->addHeader("X-Frame-Options",           "DENY");
    resp->addHeader("X-Content-Type-Options",    "nosniff");
    resp->addHeader("Referrer-Policy",           "strict-origin-when-cross-origin");
    resp->addHeader("Permissions-Policy",        "interest-cohort=(), camera=(), microphone=(), geolocation=()");
    resp->addHeader("Cross-Origin-Opener-Policy",   "same-origin");
    resp->addHeader("Cross-Origin-Resource-Policy", "same-origin");
    resp->addHeader(
        "Content-Security-Policy",
        "default-src 'self'; "
        "base-uri 'self'; "
        "frame-ancestors 'none'; "
        "img-src 'self' data: blob:; "
        "script-src 'self' https://analytics.micutu.com; "
        "style-src 'self' 'unsafe-inline'; "
        "font-src 'self' data:; "
        "connect-src 'self' https://analytics.micutu.com; "
        "form-action 'self'; "
        "object-src 'none'");
}

} // namespace security
