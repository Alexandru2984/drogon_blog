#pragma once

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <chrono>
#include <string>

namespace security {

// Returns the originating client IP. When the immediate peer is one of the
// trusted reverse proxies (loopback / private networks), the first hop from
// X-Forwarded-For is preferred. CF-Connecting-IP wins outright if present,
// since the production deployment sits behind Cloudflare.
std::string clientIp(const drogon::HttpRequestPtr& req);

// Cryptographically-random URL-safe token (uses libsodium's RNG).
std::string randomToken(std::size_t bytes = 32);

// Returns the configured CSRF cookie name. Centralized so frontend / backend
// stay in sync.
const std::string& csrfCookieName();

// Acquires (or returns false / 429-able state) one slot from the per-IP token
// bucket identified by `bucketName`. Different bucket names give independent
// budgets (e.g. "login" vs "register").
struct RateLimitDecision {
    bool   allowed;
    double retryAfterSeconds; // populated when !allowed
};

RateLimitDecision rateLimitTake(const std::string& bucketName,
                                const std::string& key,
                                double capacity,
                                double refillPerSecond);

// Applies CSP, HSTS, X-Frame-Options and friends to the response. Idempotent.
void applySecurityHeaders(const drogon::HttpResponsePtr& resp);

// Wires the rate limiter, CSRF guard, and response-header advices into the
// running Drogon app. Must be called after loadConfigJson and before run().
// Honours BLOG_DISABLE_RATE_LIMIT=1 to ease integration testing from a
// single client IP.
void registerAdvices();

} // namespace security
