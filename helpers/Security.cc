#include "Security.h"

#include <drogon/drogon.h>
#include <sodium.h>

#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace security {

namespace {

// Operator-configurable trust list, parsed once. Format: comma-separated
// IP prefixes (no CIDR mask — string-prefix match, kept simple
// deliberately). e.g. BLOG_TRUSTED_PROXIES="127.,::1,172.18."
//
// Why an env override at all: the built-in defaults trust *all* of
// RFC1918, but a VPC / shared-SDN deployment (AWS, GCP, DO, Hetzner
// Cloud private nets) routes attacker-tenant pods into the same
// 10.0.0.0/8 space as us. Without an explicit allowlist, an attacker
// on the same SDN can reach the container's 8092 port directly,
// inject X-Forwarded-For, and bypass the per-IP rate limit on /auth.
//
// Why default to loopback-only when env is unset: it's the safest
// behaviour for the prod deploy (nginx on 127.0.0.1). Anything else
// (docker-compose bridge networks, k8s pod IPs, …) must be declared
// explicitly so the operator knows what they're trusting.
const std::vector<std::string>& trustedPrefixes()
{
    static const std::vector<std::string> kDefault = {"127.", "::1"};
    static const std::vector<std::string> v = [] {
        const char* env = std::getenv("BLOG_TRUSTED_PROXIES");
        if (!env || !*env) return kDefault;
        std::vector<std::string> out;
        std::string cur;
        for (char c : std::string_view(env)) {
            if (c == ',') {
                if (!cur.empty()) out.push_back(cur);
                cur.clear();
            } else if (c != ' ' && c != '\t') {
                cur.push_back(c);
            }
        }
        if (!cur.empty()) out.push_back(cur);
        return out.empty() ? kDefault : out;
    }();
    return v;
}

bool isTrustedProxy(const std::string& ip)
{
    for (const auto& prefix : trustedPrefixes()) {
        if (ip.rfind(prefix, 0) == 0) return true;
    }
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

// Cookie names.
//
// Under TLS we use the `__Host-` prefix, which browsers enforce as a
// hard contract: the cookie MUST carry Secure, MUST have Path=/, and
// MUST NOT carry a Domain attribute. The last clause is the one that
// matters here — it makes the cookie un-writable from any other host,
// including sibling vhosts under the registrable domain.
//
// Why that is not paranoia for this deployment: the blog is one of ~36
// vhosts on micutu.com. A plain `csrf_token` cookie can be overwritten
// by any of them via `Set-Cookie: csrf_token=x; Domain=.micutu.com`,
// and a domain-scoped cookie shadows the host-scoped one on the way
// out. An attacker who controls (or finds an XSS in) any sibling host
// therefore controls BOTH halves of the double-submit pair and the
// CSRF guard stops guarding anything. Same argument for the session
// cookie: fixing it to a known value from a sibling is session
// fixation with extra steps.
//
// Plain names are kept when BLOG_SECURE_COOKIES is off, because
// `__Host-` without Secure is rejected outright by the browser and
// every dev / CI run is plain HTTP.
const std::string kCsrfCookiePlain  = "csrf_token";
const std::string kCsrfCookieHostPfx = "__Host-csrf_token";
const std::string kSessionCookiePlain   = "JSESSIONID";
const std::string kSessionCookieHostPfx = "__Host-JSESSIONID";

// Constant-time equality for the CSRF double-submit check. The token is not
// a long-term secret (an off-origin attacker cannot read the cookie to begin
// with), but using sodium_memcmp keeps one consistent rule across the
// codebase — every other token comparison is already constant-time — and
// removes a byte-at-a-time early-exit `==` from a request-path hot loop.
bool constTimeEq(const std::string& a, const std::string& b)
{
    if (a.size() != b.size()) return false;
    if (a.empty()) return true;
    return sodium_memcmp(a.data(), b.data(), a.size()) == 0;
}

struct Bucket {
    double tokens;
    std::chrono::steady_clock::time_point lastRefill;
};

std::mutex                              g_mu;
std::unordered_map<std::string, Bucket> g_buckets;

} // namespace

// Name of the single header that may declare the client IP, overridable
// via BLOG_CLIENT_IP_HEADER.
//
// This used to be a waterfall — CF-Connecting-IP, then X-Real-IP, then the
// last hop of X-Forwarded-For — which quietly assumed the reverse proxy
// stripped every one of those from inbound requests. It did not. nginx
// passes request headers upstream verbatim unless told otherwise, so
// anything that reached the origin directly could name its own client IP
// and get it believed, because the only peer the app ever sees is nginx on
// loopback. Reproduced against production: two requests carrying forged
// TEST-NET addresses were logged as originating from them, which is a full
// bypass of every per-IP token bucket plus a log-poisoning primitive.
//
// One configured header instead of a waterfall means a deployment states
// which hop it trusts rather than the app guessing. The edge is now
// responsible for making that header true — ops/nginx/blog-proxy.conf
// strips the client's copy and re-sets it from nginx's own $remote_addr,
// which blog-security.conf resolves through the real_ip module over
// Cloudflare's ranges.
const std::string& clientIpHeader()
{
    static const std::string v = [] {
        const char* env = std::getenv("BLOG_CLIENT_IP_HEADER");
        return (env && *env) ? std::string(env) : std::string("X-Real-IP");
    }();
    return v;
}

std::string clientIp(const drogon::HttpRequestPtr& req)
{
    return resolveClientIp(req->getPeerAddr().toIp(),
                           req->getHeader(clientIpHeader()));
}

std::string resolveClientIp(const std::string& peer,
                            const std::string& claimed)
{
    // Header claims are only considered when the connection itself came
    // from a declared reverse proxy. Anything else speaks for itself.
    if (!isTrustedProxy(peer)) return peer;
    if (claimed.empty())       return peer;

    // X-Forwarded-For is the one configurable value that may legitimately
    // be a chain (`client, proxy1, proxy2, …`). Take the LAST hop: with
    // nginx's default `$proxy_add_x_forwarded_for` the earlier entries are
    // whatever the client sent, while the last is the address the nearest
    // trusted proxy actually observed. Single-value headers fall through
    // this unchanged, since lastHop() of a comma-free string is itself.
    const std::string hop = lastHop(claimed);
    return hop.empty() ? peer : hop;
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

const std::string& csrfCookieName()
{
    return secureCookies() ? kCsrfCookieHostPfx : kCsrfCookiePlain;
}

const std::string& sessionCookieName()
{
    return secureCookies() ? kSessionCookieHostPfx : kSessionCookiePlain;
}

void issueCsrfCookie(const drogon::HttpRequestPtr& req,
                     const drogon::HttpResponsePtr& resp)
{
    auto session = req->session();
    std::string token;
    auto existing = session->getOptional<std::string>("csrf_token");
    if (existing.has_value() && !existing.value().empty()) {
        token = existing.value();
    } else {
        token = randomToken();
        session->insert("csrf_token", token);
    }

    drogon::Cookie c(csrfCookieName(), token);
    c.setPath("/");
    c.setHttpOnly(false);                       // frontend reads it to echo in header
    c.setSameSite(drogon::Cookie::SameSite::kLax);
    c.setSecure(secureCookies());
    resp->addCookie(std::move(c));

    // Rollout hygiene: a browser that held a session from before the
    // `__Host-` switch still carries the legacy `csrf_token`. We never
    // *accept* it (the guard below reads only csrfCookieName()), but
    // leaving it in the jar means the SPA's cookie reader can pick the
    // stale value and every mutating request 403s until the user
    // manually clears cookies. Expire it explicitly instead.
    if (secureCookies() && !req->getCookie(kCsrfCookiePlain).empty()) {
        drogon::Cookie stale(kCsrfCookiePlain, "");
        stale.setPath("/");
        stale.setHttpOnly(false);
        stale.setMaxAge(0);
        resp->addCookie(std::move(stale));
    }
}

RateLimitDecision rateLimitTake(const std::string& bucketName,
                                const std::string& key,
                                double capacity,
                                double refillPerSecond)
{
    const std::string composite = bucketName + "|" + key;
    auto now = std::chrono::steady_clock::now();

    std::lock_guard<std::mutex> lk(g_mu);

    // Opportunistic GC. Without this, every distinct (rule, ip) tuple
    // we ever see is retained for the lifetime of the process — a
    // long-running prod (or an attacker rotating IPs) grows g_buckets
    // monotonically. We sweep when the map crosses a threshold and
    // drop anything that hasn't been touched in the last hour; even
    // the slowest bucket we have refills well within that window, so
    // an evicted entry is equivalent to a fresh one.
    constexpr std::size_t kGcThreshold = 4096;
    if (g_buckets.size() >= kGcThreshold) {
        const auto cutoff = now - std::chrono::hours(1);
        for (auto bit = g_buckets.begin(); bit != g_buckets.end(); ) {
            if (bit->second.lastRefill < cutoff) bit = g_buckets.erase(bit);
            else                                 ++bit;
        }
    }

    auto makeDecision = [&](bool allowed, double tokensAfter, double retryAfter) {
        RateLimitDecision d{};
        d.allowed          = allowed;
        d.retryAfterSeconds= retryAfter;
        d.limit            = capacity;
        d.remaining        = std::max(0.0, tokensAfter);
        // Seconds until the bucket would refill back to full capacity
        // from its current `tokens` level. Clients echo this on a
        // `Retry-After`-style poll loop.
        d.resetSeconds     = (capacity - tokensAfter) / refillPerSecond;
        if (d.resetSeconds < 0.0) d.resetSeconds = 0.0;
        return d;
    };

    auto it = g_buckets.find(composite);
    if (it == g_buckets.end()) {
        const double tokensAfter = capacity - 1.0;
        g_buckets.emplace(composite, Bucket{tokensAfter, now});
        return makeDecision(true, tokensAfter, 0.0);
    }

    Bucket& b = it->second;
    double elapsed = std::chrono::duration<double>(now - b.lastRefill).count();
    b.tokens = std::min(capacity, b.tokens + elapsed * refillPerSecond);
    b.lastRefill = now;

    if (b.tokens >= 1.0) {
        b.tokens -= 1.0;
        return makeDecision(true, b.tokens, 0.0);
    }
    return makeDecision(false, b.tokens, (1.0 - b.tokens) / refillPerSecond);
}

drogon::HttpResponsePtr rateLimitOr429(const std::string& bucketName,
                                       const std::string& key,
                                       double capacity,
                                       double refillPerSecond)
{
    const char* dis = std::getenv("BLOG_DISABLE_RATE_LIMIT");
    if (dis && std::string(dis) == "1") return nullptr;
    auto d = rateLimitTake(bucketName, key, capacity, refillPerSecond);
    if (d.allowed) return nullptr;
    Json::Value body;
    body["error"] = "Too many requests";
    auto resp = drogon::HttpResponse::newHttpJsonResponse(body);
    resp->setStatusCode(drogon::k429TooManyRequests);
    resp->addHeader("Retry-After",
        std::to_string(static_cast<int>(d.retryAfterSeconds) + 1));
    return resp;
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
                            // Same advisory headers on 429 as on 200, so a
                            // client knows the budget shape without having
                            // to win a request first.
                            resp->addHeader("X-RateLimit-Limit",
                                std::to_string(static_cast<int>(d.limit)));
                            resp->addHeader("X-RateLimit-Remaining", "0");
                            resp->addHeader("X-RateLimit-Reset",
                                std::to_string(static_cast<int>(d.resetSeconds) + 1));
                            return resp;
                        }
                        // Stash the post-take bucket state on the request
                        // so the post-handling advice below can emit
                        // X-RateLimit-* without re-doing the lookup.
                        req->getAttributes()->insert(
                            "rate_limit_decision", d);
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
                if (cookieTok.empty() || !constTimeEq(cookieTok, headerTok)) {
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
        [](const drogon::HttpRequestPtr& req, const drogon::HttpResponsePtr& resp) {
            applySecurityHeaders(resp);

            // X-RateLimit-* advisory headers. The sync advice above
            // stashes the post-take state on the request whenever the
            // path was rate-limited; we surface it on every response
            // shape (200, 4xx, 5xx) so a client that hits a validation
            // error still sees its budget.
            if (req->getAttributes()->find("rate_limit_decision")) {
                const auto& d = req->getAttributes()->get<RateLimitDecision>(
                    "rate_limit_decision");
                resp->addHeader("X-RateLimit-Limit",
                    std::to_string(static_cast<int>(d.limit)));
                resp->addHeader("X-RateLimit-Remaining",
                    std::to_string(static_cast<int>(d.remaining)));
                resp->addHeader("X-RateLimit-Reset",
                    std::to_string(static_cast<int>(d.resetSeconds) + 1));
            }
        });

    // Drogon attaches the session cookie *after* PostHandling, so we patch it
    // at the very last step before serialization. When BLOG_SECURE_COOKIES=1
    // the session cookie is re-emitted with the Secure flag — which is also
    // what makes the `__Host-` prefix on its name legal.
    drogon::app().registerPreSendingAdvice(
        [](const drogon::HttpRequestPtr& req, const drogon::HttpResponsePtr& resp) {
            if (!secureCookies()) return;
            const auto& name = sessionCookieName();
            const auto& jar  = resp->getCookies();
            auto it = jar.find(name);
            if (it != jar.end() && !it->second.value().empty() &&
                !it->second.isSecure())
            {
                drogon::Cookie c = it->second;
                c.setSecure(true);
                resp->removeCookie(name);
                resp->addCookie(std::move(c));
            }

            // Same rollout hygiene as the CSRF cookie: drop the legacy
            // unprefixed session cookie so the browser stops sending two
            // competing session ids. The old one is already dead
            // server-side (Drogon keys the store on the new name), it
            // just wastes a header and confuses debugging.
            if (name != kSessionCookiePlain &&
                !req->getCookie(kSessionCookiePlain).empty())
            {
                drogon::Cookie stale(kSessionCookiePlain, "");
                stale.setPath("/");
                stale.setHttpOnly(true);
                stale.setMaxAge(0);
                resp->addCookie(std::move(stale));
            }
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

// ---- Encryption-at-rest for opaque secrets (TOTP shared keys) ----

bool validTotpKeyEncoding(std::string_view encoded)
{
    if (encoded.size() != crypto_secretbox_KEYBYTES * 2) return false;
    for (const char c : encoded) {
        const bool decimal = c >= '0' && c <= '9';
        const bool lower   = c >= 'a' && c <= 'f';
        const bool upper   = c >= 'A' && c <= 'F';
        if (!decimal && !lower && !upper) return false;
    }
    return true;
}

namespace {

// Parse BLOG_TOTP_KEY as 64 hex chars. An unset key is allowed only for
// explicit non-TLS development; a configured-but-invalid key must never be
// confused with an absent one, because that would silently write new TOTP
// seeds in plaintext.
std::optional<std::array<unsigned char, crypto_secretbox_KEYBYTES>>
loadTotpKey()
{
    const char* env = std::getenv("BLOG_TOTP_KEY");
    if (!env || !*env) {
        if (secureCookies()) {
            throw std::runtime_error(
                "BLOG_TOTP_KEY is required when BLOG_SECURE_COOKIES=1");
        }
        return std::nullopt;
    }
    std::string_view s(env);
    if (!validTotpKeyEncoding(s)) {
        throw std::runtime_error(
            "BLOG_TOTP_KEY must be exactly 64 hexadecimal characters");
    }
    std::array<unsigned char, crypto_secretbox_KEYBYTES> key{};
    auto hexVal = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (std::size_t i = 0; i < crypto_secretbox_KEYBYTES; ++i) {
        const int hi = hexVal(s[2 * i]);
        const int lo = hexVal(s[2 * i + 1]);
        key[i] = static_cast<unsigned char>((hi << 4) | lo);
    }
    return key;
}

// Base64 alphabet (standard, with padding) — local to keep this file
// self-contained; randomToken above is URL-safe and unpadded for cookies.
std::string b64Encode(const unsigned char* data, std::size_t n)
{
    static const char* kAlphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((n + 2) / 3) * 4);
    std::size_t i = 0;
    while (i + 3 <= n) {
        unsigned a = data[i], b = data[i+1], c = data[i+2];
        out.push_back(kAlphabet[(a >> 2) & 0x3F]);
        out.push_back(kAlphabet[((a & 0x3) << 4) | ((b >> 4) & 0xF)]);
        out.push_back(kAlphabet[((b & 0xF) << 2) | ((c >> 6) & 0x3)]);
        out.push_back(kAlphabet[c & 0x3F]);
        i += 3;
    }
    if (i < n) {
        unsigned a = data[i];
        unsigned b = (i + 1 < n) ? data[i+1] : 0;
        out.push_back(kAlphabet[(a >> 2) & 0x3F]);
        out.push_back(kAlphabet[((a & 0x3) << 4) | ((b >> 4) & 0xF)]);
        out.push_back((i + 1 < n) ? kAlphabet[(b & 0xF) << 2] : '=');
        out.push_back('=');
    }
    return out;
}

bool b64Decode(std::string_view s, std::vector<unsigned char>& out)
{
    static const std::array<signed char, 256> table = [] {
        std::array<signed char, 256> t{};
        for (auto& v : t) v = -1;
        const char* a = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (int i = 0; i < 64; ++i)
            t[static_cast<unsigned char>(a[i])] = static_cast<signed char>(i);
        return t;
    }();
    out.clear();
    out.reserve((s.size() / 4) * 3);
    int bits = 0, val = 0;
    for (char c : s) {
        if (c == '=' || c == '\n' || c == '\r') continue;
        signed char v = table[static_cast<unsigned char>(c)];
        if (v < 0) return false;
        val = (val << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<unsigned char>((val >> bits) & 0xFF));
        }
    }
    return true;
}

constexpr const char* kEncPrefix = "enc:v1:";

} // namespace

void validateTotpKeyConfiguration()
{
    // Parsing is the validation. Keeping this as a startup hook means a typo
    // fails the deployment immediately instead of waiting until the first
    // user enrols or verifies 2FA.
    (void)loadTotpKey();
}

std::string wrapTotpSecret(const std::string& plaintext)
{
    auto keyOpt = loadTotpKey();
    if (!keyOpt) return plaintext;       // dev / test deploy without a key

    std::array<unsigned char, crypto_secretbox_NONCEBYTES> nonce{};
    randombytes_buf(nonce.data(), nonce.size());

    std::vector<unsigned char> ct(plaintext.size() + crypto_secretbox_MACBYTES);
    if (crypto_secretbox_easy(
            ct.data(),
            reinterpret_cast<const unsigned char*>(plaintext.data()),
            plaintext.size(),
            nonce.data(),
            keyOpt->data()) != 0)
    {
        throw std::runtime_error("crypto_secretbox_easy failed");
    }

    // Pack nonce || ciphertext into a single base64 blob; the version
    // tag lets future schema rolls coexist.
    std::vector<unsigned char> combined;
    combined.reserve(nonce.size() + ct.size());
    combined.insert(combined.end(), nonce.begin(), nonce.end());
    combined.insert(combined.end(), ct.begin(), ct.end());
    return std::string(kEncPrefix) + b64Encode(combined.data(), combined.size());
}

std::string unwrapTotpSecret(const std::string& stored)
{
    // Validate first even for legacy plaintext rows. Production must not keep
    // accepting those merely because the operator omitted the encryption key.
    auto keyOpt = loadTotpKey();
    if (stored.rfind(kEncPrefix, 0) != 0) {
        // Plaintext (legacy or running without a key) — pass through.
        return stored;
    }
    if (!keyOpt) {
        throw std::runtime_error(
            "TOTP secret is encrypted but BLOG_TOTP_KEY is not set");
    }
    std::vector<unsigned char> combined;
    if (!b64Decode(std::string_view(stored).substr(std::strlen(kEncPrefix)),
                   combined)) {
        throw std::runtime_error("TOTP ciphertext base64 decode failed");
    }
    if (combined.size() < crypto_secretbox_NONCEBYTES + crypto_secretbox_MACBYTES) {
        throw std::runtime_error("TOTP ciphertext truncated");
    }
    const std::size_t ctLen = combined.size() - crypto_secretbox_NONCEBYTES;
    std::vector<unsigned char> pt(ctLen - crypto_secretbox_MACBYTES);
    if (crypto_secretbox_open_easy(
            pt.data(),
            combined.data() + crypto_secretbox_NONCEBYTES,
            ctLen,
            combined.data(),
            keyOpt->data()) != 0)
    {
        throw std::runtime_error("TOTP MAC verification failed");
    }
    return std::string(pt.begin(), pt.end());
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
        // Cloudflare Browser Insights, by origin only: the beacon is an
        // external script and it reports to /cdn-cgi/rum on this origin,
        // which 'self' already covers. The one Cloudflare script still
        // blocked is Bot Management's JS Detections bootstrap, which is
        // inline and carries a fresh ray id per request — no static hash
        // can match it, and the alternative is 'unsafe-inline'. See
        // ops/nginx/blog-security.conf; keep the two in sync.
        "script-src 'self' https://analytics.micutu.com "
            "https://static.cloudflareinsights.com; "
        "style-src 'self' 'unsafe-inline'; "
        "font-src 'self' data:; "
        "connect-src 'self' https://analytics.micutu.com; "
        "form-action 'self'; "
        "object-src 'none'");
}

} // namespace security
