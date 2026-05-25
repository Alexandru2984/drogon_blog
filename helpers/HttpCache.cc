#include "HttpCache.h"

#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>

namespace http_cache {

namespace {

// FNV-1a 64-bit. Good for a non-cryptographic resource-version hash:
// uniform over typical input, cheap, no dependencies. Collisions across
// genuinely-distinct ETag inputs would cause stale serves; the input
// shape (max_updated_at + count + cursor) makes a same-hash collision
// vanishingly unlikely in practice.
std::uint64_t fnv1a(std::string_view s)
{
    std::uint64_t h = 0xcbf29ce484222325ULL;
    for (unsigned char c : s) {
        h ^= c;
        h *= 0x100000001b3ULL;
    }
    return h;
}

std::string toHex16(std::uint64_t v)
{
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016lx", static_cast<unsigned long>(v));
    return buf;
}

// Strip surrounding whitespace and an optional W/ prefix, then return
// the inner quoted bytes. Returns empty string_view on shapes that
// don't look like a valid entity-tag.
std::string_view stripTag(std::string_view tok)
{
    while (!tok.empty() && (tok.front() == ' ' || tok.front() == '\t')) tok.remove_prefix(1);
    while (!tok.empty() && (tok.back()  == ' ' || tok.back()  == '\t')) tok.remove_suffix(1);
    if (tok.size() >= 2 && tok[0] == 'W' && tok[1] == '/') tok.remove_prefix(2);
    if (tok.size() >= 2 && tok.front() == '"' && tok.back() == '"') {
        tok.remove_prefix(1);
        tok.remove_suffix(1);
    }
    return tok;
}

} // namespace

std::string makeWeakEtag(std::initializer_list<std::string_view> parts)
{
    // Concatenate parts with a separator that cannot appear inside the
    // values we feed (timestamps, ints, percent-encoded strings) so a
    // boundary cannot be mistaken for content.
    std::string joined;
    joined.reserve(64);
    for (auto p : parts) {
        joined.append(p.data(), p.size());
        joined.push_back('\x1f');     // ASCII unit separator
    }
    return std::string("W/\"") + toHex16(fnv1a(joined)) + '"';
}

std::int64_t parseTimestampMicros(std::string_view isoUtc)
{
    // Accept "YYYY-MM-DD HH:MM:SS[.frac][+ZZ]" or "YYYY-MM-DDTHH:MM:SS…".
    // We only consume up to the second precision via strptime and then
    // pick up the fractional part manually — strptime has no portable
    // %f equivalent.
    if (isoUtc.size() < 19) return 0;
    std::tm tm{};
    std::string s(isoUtc.substr(0, 19));
    if (s[10] == 'T') s[10] = ' ';
    if (strptime(s.c_str(), "%Y-%m-%d %H:%M:%S", &tm) == nullptr) return 0;

    std::int64_t micros = 0;
    if (isoUtc.size() > 19 && isoUtc[19] == '.') {
        // up to 6 digits of fractional seconds
        std::size_t i = 20;
        std::int64_t frac = 0;
        int mul = 100000;
        while (i < isoUtc.size() && isoUtc[i] >= '0' && isoUtc[i] <= '9' && mul > 0) {
            frac += static_cast<std::int64_t>(isoUtc[i] - '0') * mul;
            mul /= 10;
            ++i;
        }
        micros = frac;
    }

    // timegm: treats tm as UTC. Postgres TIMESTAMPTZ values come back
    // expressed in the session timezone, which we leave as UTC.
    std::time_t epoch = timegm(&tm);
    if (epoch == static_cast<std::time_t>(-1)) return 0;
    return static_cast<std::int64_t>(epoch) * 1000000LL + micros;
}

bool ifNoneMatchHit(const drogon::HttpRequestPtr& req, std::string_view etag)
{
    const std::string& header = req->getHeader("If-None-Match");
    if (header.empty()) return false;

    std::string_view want = stripTag(etag);
    if (want.empty()) return false;

    // Split on commas. RFC 7232 allows whitespace between tokens.
    std::size_t i = 0;
    while (i < header.size()) {
        std::size_t j = header.find(',', i);
        std::string_view tok = std::string_view(header).substr(
            i, (j == std::string::npos) ? std::string::npos : (j - i));
        std::string_view inner = stripTag(tok);
        if (inner == "*" || inner == want) return true;
        if (j == std::string::npos) break;
        i = j + 1;
    }
    return false;
}

void applyCacheHeaders(const drogon::HttpResponsePtr& resp,
                       std::string_view etag,
                       int maxAgeSeconds,
                       std::string_view varyHeader)
{
    resp->addHeader("ETag", std::string(etag));

    // public  → intermediaries (Cloudflare, nginx proxy_cache) may store
    // max-age=N → freshness window before the next revalidation
    // must-revalidate → after expiry, a cache must re-check, not serve stale
    //
    // For per-user responses we downgrade `public` to `private` to keep
    // shared caches from holding the body at all — the Vary alone is
    // belt-and-braces but a misconfigured intermediary that ignores it
    // would otherwise be a privacy leak.
    const bool perUser = !varyHeader.empty();
    char buf[96];
    std::snprintf(buf, sizeof(buf),
        "%s, max-age=%d, must-revalidate",
        perUser ? "private" : "public",
        maxAgeSeconds);
    resp->addHeader("Cache-Control", buf);

    if (!varyHeader.empty()) {
        resp->addHeader("Vary", std::string(varyHeader));
    }
}

drogon::HttpResponsePtr makeNotModified(std::string_view etag,
                                        int maxAgeSeconds,
                                        std::string_view varyHeader)
{
    auto r = drogon::HttpResponse::newHttpResponse();
    r->setStatusCode(drogon::k304NotModified);
    applyCacheHeaders(r, etag, maxAgeSeconds, varyHeader);
    return r;
}

} // namespace http_cache
