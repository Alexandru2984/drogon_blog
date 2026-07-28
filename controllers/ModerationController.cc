#include "ModerationController.h"

#include "../helpers/AuditLog.h"
#include "../helpers/Roles.h"
#include "../helpers/Security.h"
#include "../helpers/Sessions.h"
#include "../helpers/Workers.h"

#include <drogon/orm/Exception.h>
#include <trantor/utils/Logger.h>

#include <optional>
#include <string>

using namespace drogon;
using namespace drogon::orm;

namespace {

HttpResponsePtr jsonError(HttpStatusCode code, const std::string& msg)
{
    Json::Value body;
    body["error"] = msg;
    auto resp = HttpResponse::newHttpJsonResponse(body);
    resp->setStatusCode(code);
    return resp;
}

std::optional<int> currentUserId(const HttpRequestPtr& req)
{
    auto session = req->session();
    if (!session) return std::nullopt;
    return session->getOptional<int>("user_id");
}

// Bound on the free-text field. It is attacker-controlled, gets stored and
// then rendered in the moderation queue, so it needs a ceiling; 2 KiB is
// far more than a useful report needs.
constexpr std::size_t kMaxDetail = 2048;

bool validTargetType(const std::string& t)
{
    return t == "post" || t == "comment" || t == "user";
}

bool validReason(const std::string& r)
{
    return r == "spam" || r == "harassment" || r == "illegal" ||
           r == "sexual" || r == "other";
}

// Confirms the reported thing exists before a row is written. Without it
// the queue fills with reports against ids that were never real, and the
// endpoint doubles as a way to probe which ids exist — the answer here is
// the same 404 either way.
bool targetExists(const std::string& type, int id)
{
    auto db = drogon::app().getDbClient();
    const char* sql =
        type == "post"    ? "SELECT 1 FROM posts    WHERE id = $1" :
        type == "comment" ? "SELECT 1 FROM comments WHERE id = $1" :
                            "SELECT 1 FROM users    WHERE id = $1";
    return !db->execSqlSync(sql, id).empty();
}

// Shared body for the four hide/unhide endpoints, which differ only in
// table and direction.
void setHidden(const HttpRequestPtr& req,
               std::function<void(const HttpResponsePtr&)> callback,
               const char* table, int id, bool hide)
{
    workers::offload(workers::Pool::Auth, callback,
        [req, callback, table, id, hide] {
            if (auto denied = roles::require(req, roles::Role::Moderator)) {
                callback(denied);
                return;
            }
            auto moderatorId = currentUserId(req);
            auto json = req->getJsonObject();
            std::string reason;
            if (json && json->isMember("reason")) {
                reason = (*json)["reason"].asString();
                if (reason.size() > kMaxDetail) reason.resize(kMaxDetail);
            }

            const std::string sql = hide
                ? std::string("UPDATE ") + table +
                  " SET hidden_at = NOW(), hidden_by = $2, hidden_reason = $3 "
                  " WHERE id = $1 AND hidden_at IS NULL RETURNING id"
                : std::string("UPDATE ") + table +
                  " SET hidden_at = NULL, hidden_by = NULL, hidden_reason = NULL "
                  " WHERE id = $1 AND hidden_at IS NOT NULL RETURNING id";

            try {
                auto db = drogon::app().getDbClient();
                const auto r = hide
                    ? db->execSqlSync(sql, id, *moderatorId, reason)
                    : db->execSqlSync(sql, id);
                if (r.empty()) {
                    // Either no such row, or already in the requested
                    // state. Both are "nothing to do" from the caller's
                    // point of view.
                    callback(jsonError(k404NotFound,
                                       "No such item, or already in that state"));
                    return;
                }

                Json::Value meta;
                meta["table"]  = table;
                meta["reason"] = reason;
                audit_log::record(req,
                    {hide ? "moderation.hide" : "moderation.unhide",
                     moderatorId, std::string(table), id, std::move(meta)});

                Json::Value ret;
                ret["message"] = hide ? "Hidden" : "Restored";
                callback(HttpResponse::newHttpJsonResponse(ret));
            } catch (const DrogonDbException& e) {
                LOG_ERROR << "moderation hide failed: " << e.base().what();
                callback(jsonError(k500InternalServerError, "Moderation failed"));
            }
        });
}

} // namespace

// =========================================================================
// POST /reports
// =========================================================================
void ModerationController::createReport(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback)
{
    auto userIdOpt = currentUserId(req);
    if (!userIdOpt) {
        callback(jsonError(k401Unauthorized, "Not authenticated"));
        return;
    }
    auto json = req->getJsonObject();
    if (!json) { callback(jsonError(k400BadRequest, "Invalid JSON")); return; }

    const std::string type   = (*json)["target_type"].asString();
    const int         target = (*json)["target_id"].asInt();
    const std::string reason = (*json)["reason"].asString();
    std::string       detail = (*json)["detail"].asString();

    if (!validTargetType(type)) {
        callback(jsonError(k400BadRequest,
                           "target_type must be post, comment or user"));
        return;
    }
    if (!validReason(reason)) {
        callback(jsonError(k400BadRequest,
                           "reason must be spam, harassment, illegal, sexual or other"));
        return;
    }
    if (detail.size() > kMaxDetail) detail.resize(kMaxDetail);

    // Reporting is cheap to do and expensive to review, which makes it a
    // natural griefing tool — a handful of accounts can bury a queue.
    if (auto rl = security::rateLimitOr429(
            "report_create", "uid:" + std::to_string(*userIdOpt),
            10.0, 10.0 / 3600.0)) {
        callback(rl);
        return;
    }

    workers::offload(workers::Pool::Auth, callback,
        [req, callback, userIdOpt, type, target, reason, detail] {
            try {
                if (!targetExists(type, target)) {
                    callback(jsonError(k404NotFound, "No such item"));
                    return;
                }

                auto db = drogon::app().getDbClient();
                // ON CONFLICT DO NOTHING against the partial unique index
                // on open reports: filing the same complaint twice is a
                // no-op rather than an error, because from the reporter's
                // side it already worked the first time and telling them
                // otherwise just invites a retry.
                const auto r = db->execSqlSync(
                    "INSERT INTO reports (reporter_id, target_type, target_id, "
                    "                     reason, detail) "
                    "VALUES ($1, $2, $3, $4, $5) "
                    "ON CONFLICT DO NOTHING "
                    "RETURNING id",
                    *userIdOpt, type, target, reason, detail);

                Json::Value meta;
                meta["target_type"] = type;
                meta["reason"]      = reason;
                meta["duplicate"]   = r.empty();
                audit_log::record(req, {"report.create", userIdOpt,
                                        type, target, std::move(meta)});

                Json::Value ret;
                ret["message"] = "Report received";
                auto resp = HttpResponse::newHttpJsonResponse(ret);
                resp->setStatusCode(k201Created);
                callback(resp);
            } catch (const DrogonDbException& e) {
                LOG_ERROR << "report insert failed: " << e.base().what();
                callback(jsonError(k500InternalServerError,
                                   "Could not file report"));
            }
        });
}

// =========================================================================
// GET /admin/reports?status=open
// =========================================================================
void ModerationController::listReports(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback)
{
    std::string status = req->getParameter("status");
    if (status.empty()) status = "open";
    if (status != "open" && status != "actioned" && status != "dismissed") {
        callback(jsonError(k400BadRequest, "Unknown status filter"));
        return;
    }

    workers::offload(workers::Pool::Auth, callback,
        [req, callback, status] {
            if (auto denied = roles::require(req, roles::Role::Moderator)) {
                callback(denied);
                return;
            }
            try {
                auto db = drogon::app().getDbClient();
                const auto r = db->execSqlSync(
                    "SELECT r.id, r.target_type, r.target_id, r.reason, "
                    "       r.detail, r.status, r.created_at, "
                    "       u.username AS reporter "
                    "  FROM reports r "
                    "  LEFT JOIN users u ON u.id = r.reporter_id "
                    " WHERE r.status = $1 "
                    " ORDER BY r.created_at ASC "
                    " LIMIT 200",
                    status);

                Json::Value arr(Json::arrayValue);
                for (const auto& row : r) {
                    Json::Value e;
                    e["id"]          = row["id"].as<int>();
                    e["target_type"] = row["target_type"].as<std::string>();
                    e["target_id"]   = row["target_id"].as<int>();
                    e["reason"]      = row["reason"].as<std::string>();
                    e["detail"]      = row["detail"].as<std::string>();
                    e["status"]      = row["status"].as<std::string>();
                    e["created_at"]  = row["created_at"].as<std::string>();
                    e["reporter"]    = row["reporter"].isNull()
                                          ? "" : row["reporter"].as<std::string>();
                    arr.append(e);
                }

                Json::Value ret;
                ret["reports"] = arr;
                auto resp = HttpResponse::newHttpJsonResponse(ret);
                resp->addHeader("Cache-Control", "private, no-store");
                resp->addHeader("Vary", "Cookie");
                callback(resp);
            } catch (const DrogonDbException& e) {
                LOG_ERROR << "report list failed: " << e.base().what();
                callback(jsonError(k500InternalServerError,
                                   "Could not list reports"));
            }
        });
}

// =========================================================================
// POST /admin/reports/{id}/resolve   { "status": "actioned"|"dismissed",
//                                      "note": "..." }
// =========================================================================
void ModerationController::resolveReport(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback,
    int reportId)
{
    auto json = req->getJsonObject();
    if (!json) { callback(jsonError(k400BadRequest, "Invalid JSON")); return; }

    const std::string status = (*json)["status"].asString();
    if (status != "actioned" && status != "dismissed") {
        callback(jsonError(k400BadRequest,
                           "status must be actioned or dismissed"));
        return;
    }
    std::string note = (*json)["note"].asString();
    if (note.size() > kMaxDetail) note.resize(kMaxDetail);

    workers::offload(workers::Pool::Auth, callback,
        [req, callback, reportId, status, note] {
            if (auto denied = roles::require(req, roles::Role::Moderator)) {
                callback(denied);
                return;
            }
            auto moderatorId = currentUserId(req);
            try {
                auto db = drogon::app().getDbClient();
                // Gated on status = 'open' so two moderators working the
                // queue at once cannot both claim the same item; the
                // second sees the 404 and moves on.
                const auto r = db->execSqlSync(
                    "UPDATE reports "
                    "   SET status = $2, resolved_by = $3, resolved_at = NOW(), "
                    "       resolution_note = $4 "
                    " WHERE id = $1 AND status = 'open' "
                    "RETURNING id",
                    reportId, status, *moderatorId, note);
                if (r.empty()) {
                    callback(jsonError(k404NotFound,
                                       "No such open report"));
                    return;
                }

                Json::Value meta;
                meta["status"] = status;
                audit_log::record(req, {"report.resolve", moderatorId,
                                        std::string("report"), reportId,
                                        std::move(meta)});

                Json::Value ret;
                ret["message"] = "Report resolved";
                callback(HttpResponse::newHttpJsonResponse(ret));
            } catch (const DrogonDbException& e) {
                LOG_ERROR << "report resolve failed: " << e.base().what();
                callback(jsonError(k500InternalServerError,
                                   "Could not resolve report"));
            }
        });
}

void ModerationController::hidePost(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback, int postId)
{
    setHidden(req, std::move(callback), "posts", postId, true);
}

void ModerationController::unhidePost(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback, int postId)
{
    setHidden(req, std::move(callback), "posts", postId, false);
}

void ModerationController::hideComment(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback, int commentId)
{
    setHidden(req, std::move(callback), "comments", commentId, true);
}

void ModerationController::unhideComment(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback, int commentId)
{
    setHidden(req, std::move(callback), "comments", commentId, false);
}

// =========================================================================
// POST /admin/users/{id}/ban   { "days": 7, "reason": "..." }
// =========================================================================
void ModerationController::banUser(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback,
    int targetUserId)
{
    auto json = req->getJsonObject();
    if (!json) { callback(jsonError(k400BadRequest, "Invalid JSON")); return; }

    const int days = json->isMember("days") ? (*json)["days"].asInt() : 7;
    if (days < 1 || days > 3650) {
        callback(jsonError(k400BadRequest, "days must be between 1 and 3650"));
        return;
    }
    std::string reason = (*json)["reason"].asString();
    if (reason.size() > kMaxDetail) reason.resize(kMaxDetail);

    workers::offload(workers::Pool::Auth, callback,
        [req, callback, targetUserId, days, reason] {
            if (auto denied = roles::require(req, roles::Role::Moderator)) {
                callback(denied);
                return;
            }
            auto moderatorId = currentUserId(req);
            if (moderatorId && *moderatorId == targetUserId) {
                callback(jsonError(k400BadRequest, "You cannot ban yourself"));
                return;
            }
            try {
                auto db = drogon::app().getDbClient();

                // Staff cannot be banned through this endpoint. Without
                // the guard, one compromised moderator account can lock
                // out every other moderator and the admins, turning a
                // single account takeover into a takeover of the whole
                // moderation surface.
                const auto who = db->execSqlSync(
                    "SELECT role FROM users WHERE id = $1", targetUserId);
                if (who.empty()) {
                    callback(jsonError(k404NotFound, "No such user"));
                    return;
                }
                if (who[0]["role"].as<std::string>() != "user") {
                    callback(jsonError(k403Forbidden,
                                       "Staff accounts cannot be banned here"));
                    return;
                }

                db->execSqlSync(
                    "UPDATE users "
                    "   SET banned_until = NOW() + make_interval(days => $2::int), "
                    "       ban_reason = $3 "
                    " WHERE id = $1",
                    targetUserId, days, reason);

                // A ban that leaves the account signed in everywhere is
                // not a ban. Cut every session it holds.
                const int killed =
                    sessions::revokeOthers(targetUserId, "", "banned");

                Json::Value meta;
                meta["days"]             = days;
                meta["reason"]           = reason;
                meta["revoked_sessions"] = killed;
                audit_log::record(req, {"moderation.ban", moderatorId,
                                        std::string("user"), targetUserId,
                                        std::move(meta)});

                Json::Value ret;
                ret["message"]          = "User suspended";
                ret["days"]             = days;
                ret["revoked_sessions"] = killed;
                callback(HttpResponse::newHttpJsonResponse(ret));
            } catch (const DrogonDbException& e) {
                LOG_ERROR << "ban failed: " << e.base().what();
                callback(jsonError(k500InternalServerError, "Ban failed"));
            }
        });
}

void ModerationController::unbanUser(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback,
    int targetUserId)
{
    workers::offload(workers::Pool::Auth, callback,
        [req, callback, targetUserId] {
            if (auto denied = roles::require(req, roles::Role::Moderator)) {
                callback(denied);
                return;
            }
            auto moderatorId = currentUserId(req);
            try {
                auto db = drogon::app().getDbClient();
                const auto r = db->execSqlSync(
                    "UPDATE users SET banned_until = NULL, ban_reason = NULL "
                    " WHERE id = $1 AND banned_until IS NOT NULL "
                    "RETURNING id",
                    targetUserId);
                if (r.empty()) {
                    callback(jsonError(k404NotFound,
                                       "No such user, or not suspended"));
                    return;
                }
                audit_log::record(req, {"moderation.unban", moderatorId,
                                        std::string("user"), targetUserId,
                                        Json::objectValue});
                Json::Value ret;
                ret["message"] = "Suspension lifted";
                callback(HttpResponse::newHttpJsonResponse(ret));
            } catch (const DrogonDbException& e) {
                LOG_ERROR << "unban failed: " << e.base().what();
                callback(jsonError(k500InternalServerError, "Unban failed"));
            }
        });
}
