#pragma once

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <string>

// Minimal Sentry client.
//
// We don't link sentry-native (a sizable C library that vendors curl
// + breakpad + everything). For a blog the requirements are narrow:
// "when a request errors, ship the request context + a level=error
// event to Sentry." A fire-and-forget HTTP POST to the legacy
// `/api/<project>/store/` ingest endpoint covers it in ~150 lines.
//
// Activation: BLOG_SENTRY_DSN=https://<public_key>@<host>/<project_id>
// (Glitchtip uses the same DSN format). Unset → captureError() is a
// cheap no-op; the app runs without any Sentry awareness.
//
// What we send:
//   - event_id (UUID v4, libsodium-randomized)
//   - timestamp (ISO 8601, UTC)
//   - level (error / fatal / warning)
//   - logger (constant "blog")
//   - message (caller-provided)
//   - tags: route, method, status, request_id (if available)
//   - request: url, method, headers (X-Request-Id, User-Agent only)
//   - extra: trace_id
//
// What we deliberately don't send: stack traces (the C++ runtime
// would need exception-aware instrumentation; out of scope), PII
// from headers (Authorization / Cookie stripped at compose time),
// or bodies (could contain PII / credentials).
namespace sentry {

// Parse BLOG_SENTRY_DSN and open the Drogon HttpClient connection.
// Idempotent. Returns true if Sentry is now armed.
bool install();

// Tear down the client. Called from runOnQuit.
void stop();

enum class Level { Warning, Error, Fatal };

// Fire-and-forget capture for a request that produced a 5xx (or any
// other status the caller wants to escalate). The HttpResponse is
// inspected for status/headers; the request is inspected for the
// usual context fields. No-op when not armed.
void captureRequestError(const drogon::HttpRequestPtr&  req,
                         const drogon::HttpResponsePtr& resp,
                         Level                          level,
                         const std::string&             message);

} // namespace sentry
