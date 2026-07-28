// Account-security endpoints: change your password, see where you are
// signed in, and cut off sessions you do not recognise.
//
// The gap these close: Drogon keeps sessions in memory and offers no way to
// enumerate or invalidate one from outside the request holding it. So a
// user who suspected their session was stolen had nothing to do about it
// but wait out the 14-day timeout, and there was no way to change a
// password from inside the account at all — only the emailed reset flow,
// which needs mailbox access the user may be trying to get ahead of.
//
// Worse, a password change that leaves existing sessions alive is the
// specific failure users do not expect: "I changed my password" is what
// people do when they think someone else is in their account, and if the
// attacker's session survives it, the action they took to evict the
// attacker did nothing.

#include "AuthController.h"

#include "../helpers/AuditLog.h"
#include "../helpers/Security.h"
#include "../helpers/Sessions.h"
#include "../helpers/Workers.h"

#include <drogon/orm/Exception.h>
#include <trantor/utils/Logger.h>

#include <string>

using namespace drogon;
using namespace drogon::orm;

namespace {

constexpr std::size_t kMinPasswordLen = 8;
constexpr std::size_t kMaxPasswordLen = 256;

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

} // namespace

// =========================================================================
// POST /auth/change-password
// =========================================================================
void AuthController::changePassword(
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

    const std::string currentPassword = (*json)["current_password"].asString();
    const std::string newPassword     = (*json)["new_password"].asString();

    if (currentPassword.empty() || newPassword.empty()) {
        callback(jsonError(k400BadRequest,
                           "Current and new password are required"));
        return;
    }
    if (newPassword.size() < kMinPasswordLen ||
        newPassword.size() > kMaxPasswordLen) {
        callback(jsonError(k400BadRequest,
                           "Password must be at least 8 characters"));
        return;
    }
    if (newPassword == currentPassword) {
        callback(jsonError(k400BadRequest,
                           "New password must differ from the current one"));
        return;
    }

    // Re-authentication is the point of this endpoint, not a formality: a
    // hijacked session must not be able to change the password and lock the
    // real owner out. Rate-limited per account as well, because the field
    // is an oracle for the current password otherwise.
    if (auto rl = security::rateLimitOr429(
            "change_password", "uid:" + std::to_string(*userIdOpt),
            5.0, 5.0 / 300.0)) {
        callback(rl);
        return;
    }

    // Two Argon2id operations (verify the old, hash the new) plus the
    // session sweep: all blocking, none of it belongs on an IO loop.
    workers::offload(workers::Pool::Auth, callback,
        [req, callback, userIdOpt, currentPassword, newPassword] {
            auto db = drogon::app().getDbClient();
            try {
                const auto rows = db->execSqlSync(
                    "SELECT password_hash FROM users WHERE id = $1",
                    *userIdOpt);
                if (rows.empty()) {
                    callback(jsonError(k404NotFound, "User not found"));
                    return;
                }
                if (!security::verifyPassword(
                        rows[0]["password_hash"].as<std::string>(),
                        currentPassword))
                {
                    audit_log::record(req, {"password.change.fail", userIdOpt,
                                            std::nullopt, std::nullopt,
                                            Json::objectValue});
                    callback(jsonError(k403Forbidden,
                                       "Current password is incorrect"));
                    return;
                }

                db->execSqlSync(
                    "UPDATE users SET password_hash = $1 WHERE id = $2",
                    security::hashPassword(newPassword), *userIdOpt);

                // Evict every other session. This is the behaviour a user
                // changing their password is actually asking for; leaving
                // them alive would mean an attacker who already has a
                // session keeps it. The current session is deliberately
                // spared so the user is not signed out of the device they
                // are using.
                const std::string keep =
                    sessions::currentSid(req).value_or(std::string{});
                const int revoked = sessions::revokeOthers(
                    *userIdOpt, keep, "password_change");

                Json::Value meta;
                meta["revoked_sessions"] = revoked;
                audit_log::record(req, {"password.change", userIdOpt,
                                        std::nullopt, std::nullopt,
                                        std::move(meta)});

                Json::Value ret;
                ret["message"]          = "Password changed";
                ret["revoked_sessions"] = revoked;
                callback(HttpResponse::newHttpJsonResponse(ret));
            } catch (const DrogonDbException& e) {
                LOG_ERROR << "DB Error (change password): " << e.base().what();
                callback(jsonError(k500InternalServerError,
                                   "Failed to change password"));
            }
        });
}

// =========================================================================
// GET /auth/sessions
// =========================================================================
void AuthController::listSessions(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback)
{
    auto userIdOpt = currentUserId(req);
    if (!userIdOpt) {
        callback(jsonError(k401Unauthorized, "Not authenticated"));
        return;
    }

    const std::string current =
        sessions::currentSid(req).value_or(std::string{});

    workers::offload(workers::Pool::Auth, callback,
        [callback, userIdOpt, current] {
            Json::Value ret;
            ret["sessions"] = sessions::list(*userIdOpt, current);
            auto resp = HttpResponse::newHttpJsonResponse(ret);
            // Per-user and never worth reusing: this is exactly the sort of
            // response a shared cache must not keep.
            resp->addHeader("Cache-Control", "private, no-store");
            resp->addHeader("Vary", "Cookie");
            callback(resp);
        });
}

// =========================================================================
// POST /auth/sessions/revoke   { "sid": "..." }
// =========================================================================
void AuthController::revokeSession(
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

    const std::string sid = (*json)["sid"].asString();
    if (sid.empty()) {
        callback(jsonError(k400BadRequest, "sid is required"));
        return;
    }

    workers::offload(workers::Pool::Auth, callback,
        [req, callback, userIdOpt, sid] {
            // sessions::revoke scopes the UPDATE by user_id, so a guessed or
            // stolen sid belonging to someone else simply matches nothing.
            // The 404 below is therefore the same answer for "no such
            // session" and "not yours", which is what we want it to be.
            if (!sessions::revoke(*userIdOpt, sid, "user")) {
                callback(jsonError(k404NotFound, "No such active session"));
                return;
            }
            audit_log::record(req, {"session.revoke", userIdOpt,
                                    std::string{"session"}, std::nullopt,
                                    Json::objectValue});
            Json::Value ret;
            ret["message"] = "Session revoked";
            callback(HttpResponse::newHttpJsonResponse(ret));
        });
}

// =========================================================================
// POST /auth/sessions/revoke-others
// =========================================================================
void AuthController::revokeOtherSessions(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback)
{
    auto userIdOpt = currentUserId(req);
    if (!userIdOpt) {
        callback(jsonError(k401Unauthorized, "Not authenticated"));
        return;
    }

    const std::string keep =
        sessions::currentSid(req).value_or(std::string{});

    workers::offload(workers::Pool::Auth, callback,
        [req, callback, userIdOpt, keep] {
            const int revoked =
                sessions::revokeOthers(*userIdOpt, keep, "user");

            Json::Value meta;
            meta["revoked_sessions"] = revoked;
            audit_log::record(req, {"session.revoke_others", userIdOpt,
                                    std::nullopt, std::nullopt,
                                    std::move(meta)});

            Json::Value ret;
            ret["message"]          = "Other sessions signed out";
            ret["revoked_sessions"] = revoked;
            callback(HttpResponse::newHttpJsonResponse(ret));
        });
}
