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

// SHA-256 (hex). Used to store opaque tokens at rest hashed so a DB
// snapshot doesn't leak active verification / password-reset tokens.
// Not for passwords — those go through Argon2id via hashPassword().
std::string sha256Hex(const std::string& input);

// Minimal email-shape sanity check, oriented around what we MUST
// reject for safety: any control byte / whitespace lets an attacker
// inject extra SMTP headers downstream (CWE-93). Loose structural
// validation (single `@`, at least one `.` in the domain) catches the
// obviously-malformed without trying to validate RFC 5322. Used by
// every endpoint that accepts an email field bound for an outbound
// SMTP To: header.
bool emailLooksValid(const std::string& e);

// Transparent encryption-at-rest for TOTP shared secrets and other
// app-managed opaque secrets that need to be recoverable (not just
// verified — TOTP needs the raw key to compute codes).
//
// Activation: set BLOG_TOTP_KEY to 64 hex chars (32 bytes). When the
// var is set, wrap() emits `enc:v1:<base64(nonce || ciphertext)>`;
// when unset, wrap() returns the input verbatim so dev / test
// deployments don't need a key. unwrap() handles both forms
// (round-trips encrypted values when the key is set, otherwise
// expects plaintext) — which gives us a lazy migration: existing
// rows continue to verify, new writes get encrypted, the operator
// can rotate by re-issuing secrets on a future reroll.
//
// Throws std::runtime_error when the input claims to be encrypted
// but the key is missing / the MAC doesn't verify — i.e. exactly
// the cases where silently degrading to plaintext would be the
// wrong default.
std::string wrapTotpSecret(const std::string& plaintext);
std::string unwrapTotpSecret(const std::string& stored);

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

// True when BLOG_SECURE_COOKIES=1 (set in production behind TLS). Controls
// whether the auth/CSRF cookies carry the Secure flag.
bool secureCookies();

// Argon2id (via libsodium) wrappers. hashPassword may throw std::runtime_error
// on OOM. verifyPassword is constant-time within libsodium.
std::string hashPassword(const std::string& password);
bool        verifyPassword(const std::string& storedHash,
                           const std::string& candidate);

// Wires the rate limiter, CSRF guard, and response-header advices into the
// running Drogon app. Must be called after loadConfigJson and before run().
// Honours BLOG_DISABLE_RATE_LIMIT=1 to ease integration testing from a
// single client IP.
void registerAdvices();

} // namespace security
