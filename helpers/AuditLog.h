#pragma once

#include <drogon/HttpRequest.h>
#include <json/json.h>

#include <optional>
#include <string>

// Insert-only structured audit trail for sensitive actions. Writes happen
// asynchronously through Drogon's DB client (fire-and-forget); the request
// path is never blocked on an audit write. Failures surface as LOG_ERROR
// because losing audit entries is a real problem, but they do not surface
// to the user.
//
// Action keys are stable dotted strings (`login.ok`, `login.fail`,
// `password.reset`, `post.delete`, …) so dashboards and SIEM rules can
// pivot on them without parsing free-form text.
namespace audit_log {

struct Entry {
    std::string                  action;        // required, e.g. "login.ok"
    std::optional<int>           actor_id;      // session user_id when known
    std::optional<std::string>   target_kind;   // "post", "user", "comment", ...
    std::optional<std::int64_t>  target_id;
    Json::Value                  metadata = Json::objectValue;
};

// Records an audit event for the given request. `actor_ip` is resolved via
// `security::clientIp(req)` and `req_id` via the request attributes so the
// row stitches to the JSON access log + trace span produced for the same
// HTTP transaction.
void record(const drogon::HttpRequestPtr& req, Entry entry);

} // namespace audit_log
