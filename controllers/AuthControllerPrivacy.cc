// Data portability and erasure — the two things a person is entitled to do
// with an account that holds their data, and the two things this blog had
// no way to do at all.
//
// Before this, leaving meant asking the operator to run DELETE by hand, and
// getting a copy of your own writing meant scraping your own profile page.
// Neither is a process; both are the absence of one.
//
// Three decisions shape the code below.
//
// 1. Both endpoints re-verify the password. A session cookie proves that
//    somebody was signed in on this browser at some point; it does not
//    prove they are the account holder now. Downloading every private
//    message a person ever sent, and deleting everything they ever wrote,
//    are exactly the two operations where that distinction matters.
//
// 2. Erasure anonymises the users row rather than deleting it. The FK graph
//    is full of ON DELETE CASCADE, and comments.parent_id is one of them —
//    deleting a comment in the middle of a thread takes every reply
//    underneath it, and those replies belong to other people. See
//    migrations/0015_account_erasure.sql for the whole argument.
//
// 3. The export is assembled in one transaction. A user's post list and
//    their comment list read a millisecond apart can disagree with each
//    other; a "complete copy of your data" that is internally inconsistent
//    is not one.

#include "AuthController.h"

#include "../helpers/AuditLog.h"
#include "../helpers/Security.h"
#include "../helpers/Sessions.h"
#include "../helpers/Workers.h"

#include <drogon/orm/Exception.h>
#include <trantor/utils/Logger.h>

#include <chrono>
#include <ctime>
#include <future>
#include <initializer_list>
#include <set>
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

// A nullable column as JSON: absent columns become null rather than the
// empty string, because "" and "never set" are different facts and an
// export is supposed to be a faithful copy.
template <typename T>
Json::Value orNull(const Field& f)
{
    if (f.isNull()) return Json::Value(Json::nullValue);
    return Json::Value(f.as<T>());
}

// Run one SELECT and turn the rows into an array of objects, using the
// column names the query itself declares. Keeping the shape derived from
// the query means adding a column to the export is a change in one place,
// and a column added to a table but not to the query cannot silently start
// appearing in people's downloads.
//
// `numeric` names the columns to emit as JSON numbers. It is spelled out
// per call rather than inferred: the driver hands everything back as text
// and Result::oid() is private, so the alternatives were to quote every id
// and count — an export nothing can load without a second pass — or to
// guess from the column name, which is the kind of heuristic that works
// until someone adds a column called `android_id`.
Json::Value rowsToArray(const Result& rows,
                        std::initializer_list<const char*> numeric = {})
{
    const std::set<std::string> nums(numeric.begin(), numeric.end());
    const auto ncols = rows.columns();

    Json::Value out(Json::arrayValue);
    for (const auto& row : rows) {
        Json::Value obj(Json::objectValue);
        for (Result::RowSizeType i = 0; i < ncols; ++i) {
            const auto& f = row[static_cast<Row::SizeType>(i)];
            const char* name = rows.columnName(i);
            if (f.isNull())            obj[name] = Json::Value(Json::nullValue);
            else if (nums.count(name)) obj[name] =
                static_cast<Json::Int64>(f.as<std::int64_t>());
            else                       obj[name] = f.as<std::string>();
        }
        out.append(obj);
    }
    return out;
}

std::string utcNowIso()
{
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

// Only the characters a filename can carry everywhere, so the
// Content-Disposition value cannot be used to inject a header or to write
// somewhere unexpected on the way out.
std::string safeFilenamePart(const std::string& raw)
{
    std::string out;
    out.reserve(raw.size());
    for (unsigned char c : raw) {
        if (std::isalnum(c) || c == '-' || c == '_') out.push_back(static_cast<char>(c));
        else if (c == ' ' || c == '.') out.push_back('-');
    }
    if (out.empty()) out = "account";
    if (out.size() > 40) out.resize(40);
    return out;
}

// Shared by both endpoints: the password check that turns "holds a session"
// into "is the account holder". Returns the user's row on success, or fills
// `err` and returns an empty result.
//
// Deliberately *not* rate-limited per IP only: the bucket is keyed on the
// account, because the whole point is that whoever is holding the session
// may not be its owner.
bool reauthenticate(int userId, const std::string& password,
                    std::string& usernameOut, HttpResponsePtr& err)
{
    auto db = drogon::app().getDbClient();
    const auto rows = db->execSqlSync(
        "SELECT password_hash, username FROM users "
        " WHERE id = $1 AND deleted_at IS NULL",
        userId);
    if (rows.empty()) {
        err = jsonError(k404NotFound, "User not found");
        return false;
    }
    if (!security::verifyPassword(rows[0]["password_hash"].as<std::string>(),
                                  password)) {
        err = jsonError(k403Forbidden, "Password is incorrect");
        return false;
    }
    usernameOut = rows[0]["username"].as<std::string>();
    return true;
}

} // namespace

// =========================================================================
// POST /account/export   { "password": "…" }
//
// Everything the account holds, as one JSON document, in a single
// transaction.
// =========================================================================
void AuthController::exportAccountData(
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
    const std::string password = (*json)["password"].asString();
    if (password.empty()) {
        callback(jsonError(k400BadRequest, "Password is required"));
        return;
    }

    // A dozen queries over every table the account touches is not something
    // anyone needs to do twice a minute, and the password field here is an
    // oracle for the password like any other.
    if (auto rl = security::rateLimitOr429(
            "account_export", "uid:" + std::to_string(*userIdOpt),
            3.0, 3.0 / 600.0)) {
        callback(rl);
        return;
    }

    // Argon2id plus a dozen synchronous queries: none of this belongs on an
    // IO loop.
    workers::offload(workers::Pool::Auth, callback,
        [req, callback, userIdOpt, password] {
            const int uid = *userIdOpt;
            HttpResponsePtr err;
            try {
                std::string username;
                if (!reauthenticate(uid, password, username, err)) {
                    audit_log::record(req, {"account.export.fail", userIdOpt,
                                            std::nullopt, std::nullopt,
                                            Json::objectValue});
                    callback(err);
                    return;
                }

                auto client = drogon::app().getDbClient();
                Json::Value out;
                out["export_version"] = 1;
                out["generated_at"]   = utcNowIso();

                // A real transaction object, not a bare "BEGIN" on the
                // client. DbClient is a *pool*: consecutive execSqlSync
                // calls are free to land on different connections, so a
                // hand-written BEGIN opens a transaction on one connection
                // while the queries after it run outside any transaction on
                // others. Postgres says so out loud —
                // "WARNING: there is no transaction in progress" — which is
                // how this was caught.
                //
                // REPEATABLE READ so every section below sees one snapshot.
                // Without it a post created between the posts query and the
                // comments query shows up as a comment on a post that is
                // not in the file.
                auto db = client->newTransaction();
                try {
                    db->execSqlSync(
                        "SET TRANSACTION ISOLATION LEVEL REPEATABLE READ");
                    const auto acct = db->execSqlSync(
                        "SELECT id, username, email, bio, profile_image, role, "
                        "       email_verified, created_at, updated_at "
                        "  FROM users WHERE id = $1", uid);
                    if (!acct.empty()) {
                        const auto& r = acct[0];
                        Json::Value a;
                        a["id"]             = r["id"].as<int>();
                        a["username"]       = r["username"].as<std::string>();
                        a["email"]          = r["email"].as<std::string>();
                        a["bio"]            = orNull<std::string>(r["bio"]);
                        a["profile_image"]  = orNull<std::string>(r["profile_image"]);
                        a["role"]           = r["role"].as<std::string>();
                        a["email_verified"] = r["email_verified"].as<int>() != 0;
                        a["created_at"]     = r["created_at"].as<std::string>();
                        a["updated_at"]     = r["updated_at"].as<std::string>();
                        out["account"] = a;
                    }

                    // Posts carry their tags, because a post without them is
                    // not the post as it was published.
                    out["posts"] = rowsToArray(db->execSqlSync(
                        "SELECT p.id, p.title, p.content, p.created_at, p.updated_at, "
                        "       p.published_at, p.reading_minutes, p.excerpt, "
                        "       p.view_count, p.hidden_at, "
                        "       COALESCE(string_agg(t.label, ', ' ORDER BY t.label), '') AS tags "
                        "  FROM posts p "
                        "  LEFT JOIN post_tags pt ON pt.post_id = p.id "
                        "  LEFT JOIN tags t       ON t.id = pt.tag_id "
                        " WHERE p.user_id = $1 "
                        " GROUP BY p.id ORDER BY p.id", uid),
                        {"id", "reading_minutes", "view_count"});

                    out["comments"] = rowsToArray(db->execSqlSync(
                        "SELECT id, post_id, parent_id, content, created_at, updated_at "
                        "  FROM comments WHERE user_id = $1 ORDER BY id", uid),
                        {"id", "post_id", "parent_id"});

                    out["likes"] = rowsToArray(db->execSqlSync(
                        "SELECT post_id, created_at FROM likes "
                        " WHERE user_id = $1 ORDER BY post_id", uid),
                        {"post_id"});

                    out["bookmarks"] = rowsToArray(db->execSqlSync(
                        "SELECT post_id, created_at FROM bookmarks "
                        " WHERE user_id = $1 ORDER BY post_id", uid),
                        {"post_id"});

                    // Both directions, with names: "user 41" is data the
                    // person cannot use, and the usernames are already
                    // visible to them in the app.
                    out["following"] = rowsToArray(db->execSqlSync(
                        "SELECT f.followee_id, u.username, f.created_at "
                        "  FROM follows f JOIN users u ON u.id = f.followee_id "
                        " WHERE f.follower_id = $1 ORDER BY f.created_at", uid),
                        {"followee_id"});
                    out["followers"] = rowsToArray(db->execSqlSync(
                        "SELECT f.follower_id, u.username, f.created_at "
                        "  FROM follows f JOIN users u ON u.id = f.follower_id "
                        " WHERE f.followee_id = $1 ORDER BY f.created_at", uid),
                        {"follower_id"});

                    // Both sides of every conversation: a sent message
                    // without the reply it answered is not a record of
                    // anything.
                    out["messages_sent"] = rowsToArray(db->execSqlSync(
                        "SELECT m.id, m.receiver_id, u.username AS receiver_username, "
                        "       m.content, m.is_read, m.created_at "
                        "  FROM messages m LEFT JOIN users u ON u.id = m.receiver_id "
                        " WHERE m.sender_id = $1 ORDER BY m.id", uid),
                        {"id", "receiver_id", "is_read"});
                    out["messages_received"] = rowsToArray(db->execSqlSync(
                        "SELECT m.id, m.sender_id, u.username AS sender_username, "
                        "       m.content, m.is_read, m.created_at "
                        "  FROM messages m LEFT JOIN users u ON u.id = m.sender_id "
                        " WHERE m.receiver_id = $1 ORDER BY m.id", uid),
                        {"id", "sender_id", "is_read"});

                    out["notifications"] = rowsToArray(db->execSqlSync(
                        "SELECT id, kind, actor_id, post_id, comment_id, "
                        "       read_at, created_at "
                        "  FROM notifications WHERE user_id = $1 ORDER BY id", uid),
                        {"id", "actor_id", "post_id", "comment_id"});

                    out["sessions"] = rowsToArray(db->execSqlSync(
                        "SELECT created_at, last_seen_at, revoked_at, "
                        "       user_agent, ip "
                        "  FROM user_sessions WHERE user_id = $1 "
                        " ORDER BY created_at", uid));

                    // Reports the person filed. Not reports filed *about*
                    // them: those are somebody else's statement, and
                    // handing them over would deanonymise the reporter.
                    out["reports_filed"] = rowsToArray(db->execSqlSync(
                        "SELECT id, target_type, target_id, reason, detail, "
                        "       status, created_at "
                        "  FROM reports WHERE reporter_id = $1 ORDER BY id", uid),
                        {"id", "target_id"});

                } catch (...) {
                    // The transaction commits when the object goes out of
                    // scope, including on the way out of an exception. This
                    // one is read-only so a stray commit would be harmless;
                    // rolling back anyway keeps the shape identical to the
                    // erasure below, where it is not harmless at all.
                    db->rollback();
                    throw;
                }

                audit_log::record(req, {"account.export", userIdOpt,
                                        std::nullopt, std::nullopt,
                                        Json::objectValue});

                Json::StreamWriterBuilder w;
                w["indentation"] = "  ";   // a person may well read this file
                auto resp = HttpResponse::newHttpResponse();
                resp->setContentTypeCode(CT_APPLICATION_JSON);
                resp->setBody(Json::writeString(w, out));
                resp->addHeader(
                    "Content-Disposition",
                    "attachment; filename=\"blog-export-" +
                        safeFilenamePart(username) + ".json\"");
                // Every private message the person ever sent. This is the
                // last response on the site that should sit in a cache.
                resp->addHeader("Cache-Control", "private, no-store");
                resp->addHeader("Vary", "Cookie");
                callback(resp);
            } catch (const DrogonDbException& e) {
                LOG_ERROR << "DB Error (account export): " << e.base().what();
                callback(jsonError(k500InternalServerError,
                                   "Failed to build the export"));
            }
        });
}

// =========================================================================
// POST /account/delete   { "password": "…", "confirm": "<username>" }
// =========================================================================
void AuthController::deleteAccount(
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
    const std::string password = (*json)["password"].asString();
    const std::string confirm  = (*json)["confirm"].asString();
    if (password.empty()) {
        callback(jsonError(k400BadRequest, "Password is required"));
        return;
    }

    if (auto rl = security::rateLimitOr429(
            "account_delete", "uid:" + std::to_string(*userIdOpt),
            5.0, 5.0 / 600.0)) {
        callback(rl);
        return;
    }

    const std::string keepSid = sessions::currentSid(req).value_or(std::string{});

    workers::offload(workers::Pool::Auth, callback,
        [req, callback, userIdOpt, password, confirm, keepSid] {
            const int uid = *userIdOpt;
            HttpResponsePtr err;
            try {
                std::string username;
                if (!reauthenticate(uid, password, username, err)) {
                    audit_log::record(req, {"account.delete.fail", userIdOpt,
                                            std::nullopt, std::nullopt,
                                            Json::objectValue});
                    callback(err);
                    return;
                }

                // Typing the username is not security — the password above
                // is. It is there so that "delete" cannot be the result of
                // a mis-click on a confirm dialog, which is the way
                // irreversible buttons actually get pressed by accident.
                if (confirm != username) {
                    callback(jsonError(k400BadRequest,
                                       "Type your username exactly to confirm"));
                    return;
                }

                auto client = drogon::app().getDbClient();

                // One transaction, on one connection. A half-erased account
                // — content gone but the name still on it, or the reverse —
                // is worse than either outcome, and the anonymised row must
                // land with the deletions or not at all.
                //
                // newTransaction() rather than a bare "BEGIN": DbClient is a
                // pool and hands out whichever connection is free, so a
                // hand-written BEGIN would open a transaction on one
                // connection and leave every statement after it running
                // unprotected on others.
                //
                // The promise is how the handler learns whether the COMMIT
                // actually landed. Drogon issues it from the transaction's
                // destructor and reports the outcome through this callback,
                // so without waiting the response would say "deleted" before
                // anyone knew that it was.
                auto committed = std::make_shared<std::promise<bool>>();
                auto commitResult = committed->get_future();
                bool rolledBack = false;
                auto db = client->newTransaction(
                    [committed](bool ok) { committed->set_value(ok); });

                Json::Value counts(Json::objectValue);
                try {
                    auto del = [&](const char* sql) {
                        return static_cast<int>(
                            db->execSqlSync(sql, uid).affectedRows());
                    };

                    // Posts first: their comments, likes, bookmarks, tags
                    // and view rows go with them through the existing
                    // cascades, so this must run before the per-table
                    // deletions below or those would count rows twice.
                    counts["posts"] = del(
                        "DELETE FROM posts WHERE user_id = $1");

                    // Comments the person left on other people's posts.
                    // Any that have replies underneath are tombstoned
                    // instead of deleted — deleting them would cascade
                    // through comments.parent_id and take other people's
                    // replies with them.
                    counts["comments_tombstoned"] = static_cast<int>(
                        db->execSqlSync(
                            "UPDATE comments c "
                            "   SET content = '[deleted]', deleted_at = now() "
                            " WHERE c.user_id = $1 "
                            "   AND c.deleted_at IS NULL "
                            "   AND EXISTS (SELECT 1 FROM comments r "
                            "                WHERE r.parent_id = c.id)",
                            uid).affectedRows());
                    counts["comments_deleted"] = del(
                        "DELETE FROM comments WHERE user_id = $1 "
                        "  AND deleted_at IS NULL");

                    counts["likes"]     = del("DELETE FROM likes     WHERE user_id = $1");
                    counts["bookmarks"] = del("DELETE FROM bookmarks WHERE user_id = $1");
                    counts["follows"]   = del(
                        "DELETE FROM follows "
                        " WHERE follower_id = $1 OR followee_id = $1");
                    // Both sides: a conversation is not one person's, and
                    // leaving the other half in place would keep the
                    // departing person's words on the site under a name
                    // that no longer exists.
                    counts["messages"]  = del(
                        "DELETE FROM messages "
                        " WHERE sender_id = $1 OR receiver_id = $1");
                    counts["notifications"] = del(
                        "DELETE FROM notifications "
                        " WHERE user_id = $1 OR actor_id = $1");
                    counts["reports"] = del(
                        "DELETE FROM reports WHERE reporter_id = $1");
                    counts["reset_tokens"] = del(
                        "DELETE FROM password_reset_tokens WHERE user_id = $1");
                    counts["totp_secrets"] = del(
                        "DELETE FROM user_totp_secrets WHERE user_id = $1");
                    counts["recovery_codes"] = del(
                        "DELETE FROM user_recovery_codes WHERE user_id = $1");
                    counts["webauthn_credentials"] = del(
                        "DELETE FROM user_webauthn_credentials WHERE user_id = $1");

                    // The row itself. Anonymised rather than deleted, so
                    // the tombstoned comments above keep a valid user_id —
                    // the column is NOT NULL — while carrying nothing that
                    // identifies anyone.
                    //
                    // The username and email are rewritten to values
                    // derived from the id, which frees the originals: a
                    // person who leaves can come back under the same name.
                    // The password hash is set to a string Argon2id cannot
                    // parse, so no password verifies against it, ever.
                    db->execSqlSync(
                        "UPDATE users "
                        "   SET username      = 'deleted_user_' || id, "
                        "       email         = 'deleted+' || id || '@invalid.local', "
                        "       password_hash = '!erased', "
                        "       bio           = '', "
                        "       profile_image = NULL, "
                        "       email_verified = 0, "
                        "       email_verification_token = NULL, "
                        "       email_verification_expires = NULL, "
                        "       role          = 'user', "
                        "       banned_until  = NULL, "
                        "       ban_reason    = NULL, "
                        "       deleted_at    = now() "
                        " WHERE id = $1", uid);

                } catch (...) {
                    // Without this the transaction would commit on its way
                    // out of the exception — the destructor commits unless
                    // told otherwise, which is the opposite of what a
                    // half-finished erasure needs.
                    db->rollback();
                    rolledBack = true;
                    throw;
                }

                // Release the transaction so the COMMIT is issued, then wait
                // for the callback above to report the result. The timeout
                // is a deadlock guard, not a policy: a commit that has not
                // been answered in thirty seconds is a database that is not
                // going to answer.
                db.reset();
                if (!rolledBack) {
                    if (commitResult.wait_for(std::chrono::seconds(30)) !=
                            std::future_status::ready ||
                        !commitResult.get())
                    {
                        LOG_ERROR << "account erasure commit failed: uid=" << uid;
                        callback(jsonError(k500InternalServerError,
                                           "Failed to delete the account"));
                        return;
                    }
                }

                // Every session, including this one — the account is gone,
                // so there is nothing left to stay signed in to. Done after
                // the commit: revoking sessions for an erasure that then
                // rolled back would sign someone out of an account that
                // still exists.
                const int revoked = sessions::revokeOthers(uid, "", "account_deleted");
                counts["sessions_revoked"] = revoked;
                // Same shape as logout: clear the payload and rotate the id
                // so the cookie the browser is holding is worthless. Not a
                // hand-built expiry cookie — the session cookie is named
                // `__Host-…` over TLS and plain otherwise, and duplicating
                // that decision here is how the two drift apart.
                if (auto session = req->session()) {
                    session->clear();
                    session->changeSessionIdToClient();
                }

                audit_log::record(req, {"account.delete", userIdOpt,
                                        "user", static_cast<std::int64_t>(uid),
                                        counts});
                LOG_WARN << "account erased: uid=" << uid
                         << " posts=" << counts["posts"].asInt()
                         << " comments_deleted=" << counts["comments_deleted"].asInt()
                         << " comments_tombstoned=" << counts["comments_tombstoned"].asInt();

                Json::Value ret;
                ret["message"] = "Account deleted";
                ret["deleted"] = counts;
                auto resp = HttpResponse::newHttpJsonResponse(ret);
                resp->addHeader("Cache-Control", "private, no-store");
                callback(resp);
            } catch (const DrogonDbException& e) {
                LOG_ERROR << "DB Error (account delete): " << e.base().what();
                callback(jsonError(k500InternalServerError,
                                   "Failed to delete the account"));
            }
        });
}
