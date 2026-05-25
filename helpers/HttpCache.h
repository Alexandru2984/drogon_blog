#pragma once

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>

#include <chrono>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>

// HTTP caching helpers built around weak ETags + RFC 7232 conditional
// requests (If-None-Match → 304 Not Modified).
//
// Why weak ETags (W/"..."): the JSON encoder occasionally reorders
// numerically-equal floats and trims trailing zeros, and we use
// LEFT JOIN + ORDER BY id DESC which is byte-stable but ts_headline()
// (search) is not. Weak ETags promise "semantically equivalent", not
// "byte identical" — which is what we actually need for revalidation.
//
// Why server-derived (not random / not random-seeded): an ETag must be
// reproducible across server restarts and across all replicas, otherwise
// caches downstream (browser, Cloudflare, intermediate proxies) thrash
// the moment a process recycles. So we derive ours strictly from row
// metadata the clients can already see: max(updated_at), row count,
// pagination cursor, query string.
//
// Why we do NOT skip the DB query on cache hit (for now): the cost of
// the work we'd save is much smaller than the cost of double-querying
// (one cheap pre-query + one full query on cache miss). The bandwidth
// win on 304 is real on its own — typical /posts JSON is several KiB,
// the 304 response is a few hundred bytes of headers.
namespace http_cache {

// Compute a weak ETag from a list of `key=value` fragments. Output format:
//   W/"hash_8bytes_hex"
// The hex encoding keeps the header purely ASCII so nginx / Cloudflare
// never have to fight quoting. 16 hex chars = 64 bits of entropy, plenty
// for distinguishing logically-distinct revisions of the same resource.
std::string makeWeakEtag(std::initializer_list<std::string_view> parts);

// Format a UTC TIMESTAMPTZ value as microseconds-since-epoch so callers
// can pass it through makeWeakEtag() as a stable, integer-shaped fragment
// (vs. text formatting which jitters across PG / libpq versions).
//
// Accepts the ISO-8601 representation that drogon's Postgres binding
// hands back from row[...].as<std::string>() for TIMESTAMPTZ columns:
//   2026-05-24 17:42:31.123456+00
// Falls back to "0" if the string is unparseable so cache misses fail
// safe (always serve fresh) rather than crashing the handler.
std::int64_t parseTimestampMicros(std::string_view isoUtc);

// Walk the If-None-Match header and decide whether the client already
// has a fresh copy. Returns true if any of the comma-separated tokens
// matches `etag` (RFC 7232 §3.2 weak-or-strong comparison: weak tags
// match weakly, "*" matches everything).
bool ifNoneMatchHit(const drogon::HttpRequestPtr& req, std::string_view etag);

// Add `ETag` and `Cache-Control` to a 200 response. `maxAgeSeconds=0`
// keeps clients revalidating on every navigation but lets them serve
// from local cache on the same page (back-button, prefetch). Bump it
// only for resources that genuinely tolerate stale reads.
//
// `varyHeader`, when non-empty, is emitted as a `Vary:` header — set
// to "Cookie" on per-session responses (e.g. /auth/me) so an upstream
// CDN keys its cache by Cookie and can't serve one user's identity
// to another. Multiple values can be comma-joined ("Cookie, Accept").
void applyCacheHeaders(const drogon::HttpResponsePtr& resp,
                       std::string_view etag,
                       int maxAgeSeconds = 0,
                       std::string_view varyHeader = {});

// Build the canonical RFC 7232 304. Empty body, ETag echoed back,
// Cache-Control included so revalidation behaviour matches a 200.
// `varyHeader` is mirrored from the 200 response so caches keep the
// same cache-key semantics on revalidation.
drogon::HttpResponsePtr makeNotModified(std::string_view etag,
                                        int maxAgeSeconds = 0,
                                        std::string_view varyHeader = {});

} // namespace http_cache
