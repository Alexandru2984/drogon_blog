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

// X-Forwarded-For is chained `client, proxy1, proxy2, …`. The first entry is
// user-controlled (nginx default appends to whatever the client supplied)
// and trusting it would let an attacker rotate rate-limit buckets by sending
// arbitrary headers. The LAST entry is the IP that the most recent trusted
// proxy actually observed connecting to it — much harder to forge.
std::string lastHop(const std::string& xff)
{
    auto comma = xff.rfind(',');
    std::string last = (comma == std::string::npos) ? xff : xff.substr(comma + 1);
    while (!last.empty() && std::isspace(static_cast<unsigned char>(last.front()))) last.erase(last.begin());
    while (!last.empty() && std::isspace(static_cast<unsigned char>(last.back())))  last.pop_back();
    return last;
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
    std::string peer = req->getPeerAddr().toIp();
    const bool trustedPeer = isTrustedProxy(peer);

    // Production runs behind Cloudflare → nginx → Drogon. CF-Connecting-IP is
    // unspoofable (Cloudflare sets it and overwrites whatever the client
    // sent), so it wins outright when the immediate peer is a trusted proxy.
    const auto& cf = req->getHeader("CF-Connecting-IP");
    if (!cf.empty() && trustedPeer) return cf;

    // X-Real-IP is what nginx sets via `proxy_set_header X-Real-IP $remote_addr`
    // — a single value, the IP nginx itself observed. Trusted because nginx
    // strips any inbound copy before re-emitting it. Recommended over XFF.
    const auto& realIp = req->getHeader("X-Real-IP");
    if (!realIp.empty() && trustedPeer) return realIp;

    // Last resort: X-Forwarded-For. Take the LAST hop, not the first — nginx's
    // default `$proxy_add_x_forwarded_for` appends to whatever the client
    // already sent, so the first entry is attacker-controlled. The last
    // entry is the IP the most recent trusted proxy actually saw.
    const auto& xff = req->getHeader("X-Forwarded-For");
    if (!xff.empty() && trustedPeer) return lastHop(xff);

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
    // Per-IP token buckets for sensitive endpoints. Capacity is the burst
    // budget; refill rate sets the long-run cap. Tuned to be friendly to
    // humans (5 logins within a few seconds is fine) while killing credential
    // stuffing and FTS-DoS.
    //
    // method is matched explicitly so a rule's path doesn't pick up unrelated
    // verbs (e.g. /posts/search is a GET, but mutating endpoints under /auth
    // share their prefixes with safe GETs).
    struct RateRule {
        const char*    path;
        drogon::HttpMethod method;
        double         capacity;
        double         refillPerSec;
    };
    static const RateRule kRateRules[] = {
        {"/auth/login",                drogon::Post, 5.0, 5.0  / 60.0},   // 5 burst, 5 / min
        {"/auth/register",             drogon::Post, 3.0, 3.0  / 600.0},  // 3 burst, 3 / 10 min
        {"/auth/request-reset",        drogon::Post, 3.0, 3.0  / 600.0},
        {"/auth/resend-verification",  drogon::Post, 3.0, 3.0  / 600.0},
        {"/auth/reset-password",       drogon::Post, 5.0, 5.0  / 60.0},
        // Full-text search runs websearch_to_tsquery + ts_headline server
        // side and ts_headline is CPU-intensive (it walks the document
        // text, not just the index). A bot looping on /posts/search with
        // long crafted terms can saturate Postgres cores; per-IP cap kills
        // that without affecting human browsing.
        {"/posts/search",              drogon::Get,  10.0, 10.0 / 60.0},  // 10 burst, 10 / min
    };

    static const std::unordered_set<std::string> kCsrfExempt = {
        "/auth/login", "/auth/register",
        "/auth/request-reset", "/auth/reset-password",
        "/auth/verify-email", "/auth/resend-verification",
        // Two-step login completion: the user is mid-flow and does not
        // hold an authenticated session yet, so they cannot present a
        // CSRF cookie/header pair. Each of these endpoints is bound to
        // the `pending_user_id` session key planted by /auth/login,
        // which is the actual CSRF mitigation (an off-origin attacker
        // cannot create that pending state).
        "/auth/login/verify-totp",
        "/auth/login/verify-recovery",
        "/auth/login/verify-webauthn/begin",
        "/auth/login/verify-webauthn/finish",
    };
    (void)kCsrfExempt;  // referenced below; kept extracted for clarity

    const char* disableRl = std::getenv("BLOG_DISABLE_RATE_LIMIT");
    const bool rateLimitEnabled = !(disableRl && std::string(disableRl) == "1");

    drogon::app().registerSyncAdvice(
        [rateLimitEnabled](const drogon::HttpRequestPtr& req) -> drogon::HttpResponsePtr {
            const auto path   = req->getPath();
            const auto method = req->getMethod();

            // ---- Rate limit ----
            if (rateLimitEnabled) {
                for (const auto& rule : kRateRules) {
                    if (path == rule.path && method == rule.method) {
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

bool emailLooksValid(const std::string& e)
{
    if (e.empty() || e.size() > 255) return false;
    for (unsigned char c : e) {
        if (c < 0x20 || c == 0x7F) return false;
        if (c == ' ')               return false;
    }
    const auto at = e.find('@');
    if (at == std::string::npos || at == 0 || at == e.size() - 1) return false;
    if (e.find('@', at + 1) != std::string::npos) return false;
    if (e.find('.', at + 1) == std::string::npos) return false;
    return true;
}

std::string sha256Hex(const std::string& input)
{
    unsigned char digest[crypto_hash_sha256_BYTES];
    crypto_hash_sha256(digest,
        reinterpret_cast<const unsigned char*>(input.data()),
        input.size());
    static const char* kHex = "0123456789abcdef";
    std::string out;
    out.resize(crypto_hash_sha256_BYTES * 2);
    for (std::size_t i = 0; i < crypto_hash_sha256_BYTES; ++i) {
        out[2 * i]     = kHex[digest[i] >> 4];
        out[2 * i + 1] = kHex[digest[i] & 0xF];
    }
    return out;
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
