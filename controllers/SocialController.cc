#include "SocialController.h"
#include "../helpers/Notifications.h"
#include "../helpers/PostMeta.h"

#include <drogon/drogon.h>
#include <drogon/orm/Exception.h>
#include <trantor/utils/Logger.h>

#include <optional>
#include <string>

using namespace drogon;
using namespace drogon::orm;

namespace {

// Every endpoint in this controller is the reader's own; there is no
// anonymous variant of "my bookmarks". Returning the id or an empty optional
// keeps the 401 in one place.
std::optional<int> viewer(const HttpRequestPtr& req)
{
    auto session = req->session();
    if (!session) return std::nullopt;
    return session->getOptional<int>("user_id");
}

HttpResponsePtr jsonWith(const Json::Value& body, HttpStatusCode code = k200OK)
{
    auto resp = HttpResponse::newHttpJsonResponse(body);
    resp->setStatusCode(code);
    return resp;
}

HttpResponsePtr unauthenticated()
{
    Json::Value ret;
    ret["error"] = "Not authenticated";
    return jsonWith(ret, k401Unauthorized);
}

HttpResponsePtr serverError(const char* what)
{
    Json::Value ret;
    ret["error"] = what;
    return jsonWith(ret, k500InternalServerError);
}

// Shared shape for any list of posts this controller returns, so the
// bookmarks page and the following feed render with the same client code as
// the main feed.
Json::Value postRow(const Row& row, const Json::Value& tagsById)
{
    Json::Value post;
    const auto id      = row["id"].as<int64_t>();
    post["id"]         = id;
    post["title"]      = row["title"].as<std::string>();
    post["content"]    = row["content"].as<std::string>();
    if (!row["content_html"].isNull())
        post["content_html"] = row["content_html"].as<std::string>();
    post["created_at"]      = row["created_at"].as<std::string>();
    post["updated_at"]      = row["updated_at"].as<std::string>();
    post["reading_minutes"] = row["reading_minutes"].as<int>();
    post["view_count"]      = row["view_count"].as<int64_t>();
    if (!row["excerpt"].isNull())
        post["excerpt"] = row["excerpt"].as<std::string>();

    const std::string key = std::to_string(id);
    post["tags"] = tagsById.isMember(key) ? tagsById[key]
                                          : Json::Value(Json::arrayValue);

    if (!row["author_id"].isNull()) {
        post["author"]["id"]       = row["author_id"].as<int64_t>();
        post["author"]["username"] = row["author_username"].as<std::string>();
        if (!row["author_profile_image"].isNull()) {
            const auto img = row["author_profile_image"].as<std::string>();
            if (!img.empty()) post["author"]["profile_image"] = img;
        }
    }
    return post;
}

Json::Value postList(const Result& r)
{
    auto db = drogon::app().getDbClient();
    std::vector<int> ids;
    ids.reserve(r.size());
    for (const auto& row : r) ids.push_back(row["id"].as<int>());
    const Json::Value tagsById = post_meta::tagsForPosts(db, ids);

    Json::Value out(Json::arrayValue);
    for (const auto& row : r) out.append(postRow(row, tagsById));
    return out;
}

// The column list every post query in this file selects. Kept in one place
// so postRow() and the queries cannot drift apart.
constexpr const char* kPostColumns =
    "p.id, p.title, p.content, p.content_html, p.created_at, p.updated_at, "
    "p.reading_minutes, p.excerpt, p.view_count, "
    "u.id AS author_id, u.username AS author_username, "
    "u.profile_image AS author_profile_image";

} // namespace

// =========================================================== bookmarks

void SocialController::listBookmarks(const HttpRequestPtr &req,
                                     std::function<void(const HttpResponsePtr &)> &&callback)
{
    const auto me = viewer(req);
    if (!me) { callback(unauthenticated()); return; }

    auto db = drogon::app().getDbClient();
    const std::string sql =
        std::string("SELECT ") + kPostColumns + ", b.created_at AS saved_at "
        "  FROM bookmarks b "
        "  JOIN posts p ON p.id = b.post_id "
        "  LEFT JOIN users u ON u.id = p.user_id "
        // A bookmark on a post that has since been hidden or unpublished
        // should not resurface it. The row stays, so if the post comes back
        // so does the bookmark.
        " WHERE b.user_id = $1 AND p.hidden_at IS NULL AND p.published_at IS NOT NULL "
        " ORDER BY b.created_at DESC "
        " LIMIT 100";

    db->execSqlAsync(
        sql,
        [callback](const Result& r) {
            Json::Value ret;
            ret["posts"] = postList(r);
            auto resp = HttpResponse::newHttpJsonResponse(ret);
            // A reading list is nobody else's business.
            resp->addHeader("Cache-Control", "private, no-store");
            callback(resp);
        },
        [callback](const DrogonDbException& e) {
            LOG_ERROR << "DB Error (listBookmarks): " << e.base().what();
            callback(serverError("Failed to load bookmarks"));
        },
        me.value());
}

void SocialController::addBookmark(const HttpRequestPtr &req,
                                   std::function<void(const HttpResponsePtr &)> &&callback,
                                   int postId)
{
    const auto me = viewer(req);
    if (!me) { callback(unauthenticated()); return; }

    auto db = drogon::app().getDbClient();
    try {
        // ON CONFLICT DO NOTHING makes this idempotent: a double tap on the
        // bookmark button is not an error the client has to distinguish from
        // a real failure.
        //
        // INSERT … SELECT rather than VALUES so the post's visibility is part
        // of the write. A plain VALUES only had the foreign key to stop it,
        // which proves the row exists and nothing about whether the caller
        // may see it — so saving someone else's unpublished draft succeeded,
        // and the 200-vs-404 split told the caller it was there. listBookmarks
        // already filters drafts back out on read, so the row was invisible
        // but the answer was not.
        const auto r = db->execSqlSync(
            "INSERT INTO bookmarks (user_id, post_id) "
            "SELECT $1, p.id FROM posts p "
            " WHERE p.id = $2 AND p.hidden_at IS NULL "
            "   AND p.published_at IS NOT NULL "
            "ON CONFLICT DO NOTHING "
            "RETURNING post_id",
            me.value(), postId);

        // Empty means either "not visible" or "already bookmarked". Ask
        // which, rather than reporting a 404 for a post the caller has
        // legitimately saved already.
        if (r.empty()) {
            const auto exists = db->execSqlSync(
                "SELECT 1 FROM bookmarks WHERE user_id = $1 AND post_id = $2",
                me.value(), postId);
            if (exists.empty()) {
                Json::Value ret;
                ret["error"] = "Post not found";
                callback(jsonWith(ret, k404NotFound));
                return;
            }
        }

        Json::Value ret;
        ret["bookmarked"] = true;
        callback(jsonWith(ret));
    } catch (const DrogonDbException& e) {
        // A foreign-key violation here means the post does not exist. That
        // is a 404, not a 500 — the client asked about something that is not
        // there rather than triggering a fault.
        const std::string what = e.base().what();
        if (what.find("foreign key") != std::string::npos) {
            Json::Value ret;
            ret["error"] = "Post not found";
            callback(jsonWith(ret, k404NotFound));
            return;
        }
        LOG_ERROR << "DB Error (addBookmark): " << what;
        callback(serverError("Failed to save bookmark"));
    }
}

void SocialController::removeBookmark(const HttpRequestPtr &req,
                                      std::function<void(const HttpResponsePtr &)> &&callback,
                                      int postId)
{
    const auto me = viewer(req);
    if (!me) { callback(unauthenticated()); return; }

    auto db = drogon::app().getDbClient();
    try {
        db->execSqlSync("DELETE FROM bookmarks WHERE user_id = $1 AND post_id = $2",
                        me.value(), postId);
        // Idempotent in the other direction too: removing a bookmark that is
        // not there leaves the caller in the state they asked for.
        Json::Value ret;
        ret["bookmarked"] = false;
        callback(jsonWith(ret));
    } catch (const DrogonDbException& e) {
        LOG_ERROR << "DB Error (removeBookmark): " << e.base().what();
        callback(serverError("Failed to remove bookmark"));
    }
}

// ============================================================= follows

void SocialController::follow(const HttpRequestPtr &req,
                              std::function<void(const HttpResponsePtr &)> &&callback,
                              int userId)
{
    const auto me = viewer(req);
    if (!me) { callback(unauthenticated()); return; }

    if (me.value() == userId) {
        // The table's CHECK would reject this anyway; catching it here gives
        // a message that says what went wrong instead of a 500.
        Json::Value ret;
        ret["error"] = "You cannot follow yourself";
        callback(jsonWith(ret, k400BadRequest));
        return;
    }

    auto db = drogon::app().getDbClient();
    try {
        auto r = db->execSqlSync(
            "INSERT INTO follows (follower_id, followee_id) VALUES ($1, $2) "
            "ON CONFLICT DO NOTHING RETURNING 1",
            me.value(), userId);

        // Only notify on a transition. Without the RETURNING check, a client
        // that re-sends the follow (a retry, a double tap) would send the
        // author a fresh notification each time.
        if (!r.empty()) {
            notifications::emit(db, userId, me.value(), notifications::Kind::Follow);
        }
        Json::Value ret;
        ret["following"] = true;
        callback(jsonWith(ret));
    } catch (const DrogonDbException& e) {
        const std::string what = e.base().what();
        if (what.find("foreign key") != std::string::npos) {
            Json::Value ret;
            ret["error"] = "User not found";
            callback(jsonWith(ret, k404NotFound));
            return;
        }
        LOG_ERROR << "DB Error (follow): " << what;
        callback(serverError("Failed to follow"));
    }
}

void SocialController::unfollow(const HttpRequestPtr &req,
                                std::function<void(const HttpResponsePtr &)> &&callback,
                                int userId)
{
    const auto me = viewer(req);
    if (!me) { callback(unauthenticated()); return; }

    auto db = drogon::app().getDbClient();
    try {
        db->execSqlSync(
            "DELETE FROM follows WHERE follower_id = $1 AND followee_id = $2",
            me.value(), userId);
        Json::Value ret;
        ret["following"] = false;
        callback(jsonWith(ret));
    } catch (const DrogonDbException& e) {
        LOG_ERROR << "DB Error (unfollow): " << e.base().what();
        callback(serverError("Failed to unfollow"));
    }
}

void SocialController::followStats(const HttpRequestPtr &req,
                                   std::function<void(const HttpResponsePtr &)> &&callback,
                                   int userId)
{
    // Public counts; the "am I following" flag is per-viewer and is simply
    // false for an anonymous reader.
    const auto me = viewer(req);
    const int meId = me.value_or(0);

    auto db = drogon::app().getDbClient();
    db->execSqlAsync(
        "SELECT (SELECT count(*) FROM follows WHERE followee_id = $1) AS followers, "
        "       (SELECT count(*) FROM follows WHERE follower_id = $1) AS following, "
        "       EXISTS (SELECT 1 FROM follows "
        "                WHERE follower_id = $2::int AND followee_id = $1) AS is_following",
        [callback](const Result& r) {
            Json::Value ret;
            ret["followers"]    = r[0]["followers"].as<int64_t>();
            ret["following"]    = r[0]["following"].as<int64_t>();
            ret["is_following"] = r[0]["is_following"].as<bool>();
            auto resp = HttpResponse::newHttpJsonResponse(ret);
            // is_following varies per viewer, so a shared cache must not
            // hand one reader another reader's answer.
            resp->addHeader("Vary", "Cookie");
            resp->addHeader("Cache-Control", "private, max-age=0, must-revalidate");
            callback(resp);
        },
        [callback](const DrogonDbException& e) {
            LOG_ERROR << "DB Error (followStats): " << e.base().what();
            callback(serverError("Failed to load follow stats"));
        },
        userId, meId);
}

void SocialController::followingFeed(const HttpRequestPtr &req,
                                     std::function<void(const HttpResponsePtr &)> &&callback)
{
    const auto me = viewer(req);
    if (!me) { callback(unauthenticated()); return; }

    auto db = drogon::app().getDbClient();
    const std::string sql =
        std::string("SELECT ") + kPostColumns +
        "  FROM posts p "
        "  JOIN follows f ON f.followee_id = p.user_id AND f.follower_id = $1 "
        "  LEFT JOIN users u ON u.id = p.user_id "
        " WHERE p.hidden_at IS NULL AND p.published_at IS NOT NULL "
        " ORDER BY p.id DESC "
        " LIMIT 50";

    db->execSqlAsync(
        sql,
        [callback](const Result& r) {
            Json::Value ret;
            ret["posts"] = postList(r);
            auto resp = HttpResponse::newHttpJsonResponse(ret);
            resp->addHeader("Cache-Control", "private, no-store");
            callback(resp);
        },
        [callback](const DrogonDbException& e) {
            LOG_ERROR << "DB Error (followingFeed): " << e.base().what();
            callback(serverError("Failed to load feed"));
        },
        me.value());
}

// ======================================================= notifications

void SocialController::listNotifications(const HttpRequestPtr &req,
                                         std::function<void(const HttpResponsePtr &)> &&callback)
{
    const auto me = viewer(req);
    if (!me) { callback(unauthenticated()); return; }

    long long before = 0;
    try { before = std::stoll(req->getParameter("before")); } catch (...) { before = 0; }
    int limit = 50;
    try { limit = std::stoi(req->getParameter("limit")); } catch (...) { limit = 50; }

    auto db = drogon::app().getDbClient();
    Json::Value ret;
    ret["notifications"] = notifications::list(db, me.value(), limit, before);
    ret["unread"]        = static_cast<Json::Int64>(
                               notifications::unreadCount(db, me.value()));

    auto resp = HttpResponse::newHttpJsonResponse(ret);
    resp->addHeader("Cache-Control", "private, no-store");
    callback(resp);
}

void SocialController::unreadCount(const HttpRequestPtr &req,
                                   std::function<void(const HttpResponsePtr &)> &&callback)
{
    const auto me = viewer(req);
    if (!me) { callback(unauthenticated()); return; }

    auto db = drogon::app().getDbClient();
    Json::Value ret;
    ret["unread"] = static_cast<Json::Int64>(
                        notifications::unreadCount(db, me.value()));

    auto resp = HttpResponse::newHttpJsonResponse(ret);
    resp->addHeader("Cache-Control", "private, no-store");
    callback(resp);
}

void SocialController::markNotificationRead(const HttpRequestPtr &req,
                                            std::function<void(const HttpResponsePtr &)> &&callback,
                                            const std::string &id)
{
    const auto me = viewer(req);
    if (!me) { callback(unauthenticated()); return; }

    // The id is bigint, so it comes in as a string rather than an int route
    // parameter — a notification id can exceed what int32 holds.
    long long nid = 0;
    try { nid = std::stoll(id); } catch (...) { nid = 0; }
    if (nid <= 0) {
        Json::Value ret;
        ret["error"] = "Invalid notification id";
        callback(jsonWith(ret, k400BadRequest));
        return;
    }

    auto db = drogon::app().getDbClient();
    // markRead scopes to the owner in its WHERE clause, so a miss means
    // either "not yours" or "already read". Both are a no-op for the caller
    // and neither should reveal which.
    notifications::markRead(db, me.value(), nid);

    Json::Value ret;
    ret["unread"] = static_cast<Json::Int64>(
                        notifications::unreadCount(db, me.value()));
    callback(jsonWith(ret));
}

void SocialController::markAllRead(const HttpRequestPtr &req,
                                   std::function<void(const HttpResponsePtr &)> &&callback)
{
    const auto me = viewer(req);
    if (!me) { callback(unauthenticated()); return; }

    auto db = drogon::app().getDbClient();
    Json::Value ret;
    ret["marked"] = static_cast<Json::Int64>(
                        notifications::markAllRead(db, me.value()));
    ret["unread"] = 0;
    callback(jsonWith(ret));
}
