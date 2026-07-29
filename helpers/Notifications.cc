#include "Notifications.h"

#include <drogon/drogon.h>
#include <trantor/utils/Logger.h>

using namespace drogon;
using namespace drogon::orm;

namespace notifications {

const char* kindName(Kind k)
{
    switch (k) {
        case Kind::Comment: return "comment";
        case Kind::Reply:   return "reply";
        case Kind::Follow:  return "follow";
        case Kind::NewPost: return "new_post";
        case Kind::Like:    return "like";
    }
    // Unreachable for a valid enum value, but the CHECK constraint would
    // reject anything else anyway, so failing here is louder than silently
    // inserting a kind no renderer knows.
    return "comment";
}

void emit(const DbClientPtr& db,
          int recipientUserId,
          int actorUserId,
          Kind kind,
          int postId,
          int commentId)
{
    // Nobody needs telling about their own actions. Filtered centrally so no
    // call site has to remember, and so "did I just notify myself" is not a
    // question that can be answered differently in five places.
    if (recipientUserId <= 0 || recipientUserId == actorUserId) return;

    try {
        db->execSqlSync(
            "INSERT INTO notifications (user_id, actor_id, kind, post_id, comment_id) "
            "VALUES ($1, $2, $3, "
            "        NULLIF($4::int, 0), NULLIF($5::int, 0))",
            recipientUserId, actorUserId, kindName(kind), postId, commentId);
    } catch (const DrogonDbException& e) {
        // Best-effort by design: see the header. The action that triggered
        // this has already succeeded and must not be rolled back.
        LOG_ERROR << "DB Error (notifications::emit, kind=" << kindName(kind)
                  << ", to=" << recipientUserId << "): " << e.base().what();
    }
}

void emitNewPostToFollowers(const DbClientPtr& db, int authorUserId, int postId)
{
    try {
        // INSERT ... SELECT: one statement regardless of follower count. A
        // loop would put a round trip per follower inside a request handler,
        // which for a popular author is a request that never finishes.
        //
        // The follows table already forbids self-follows, so no filter for
        // that is needed here.
        db->execSqlSync(
            "INSERT INTO notifications (user_id, actor_id, kind, post_id) "
            "SELECT f.follower_id, $1, 'new_post', $2 "
            "  FROM follows f WHERE f.followee_id = $1",
            authorUserId, postId);
    } catch (const DrogonDbException& e) {
        LOG_ERROR << "DB Error (notifications::emitNewPostToFollowers, author="
                  << authorUserId << "): " << e.base().what();
    }
}

Json::Value list(const DbClientPtr& db, int userId, int limit, long long beforeId)
{
    Json::Value out(Json::arrayValue);
    if (limit < 1)   limit = 1;
    if (limit > 100) limit = 100;

    try {
        // Everything the client needs to render a line, resolved here.
        // Without the joins the UI would fetch each actor and each post
        // separately — a notification list of fifty rows becoming a hundred
        // requests.
        //
        // $3 = 0 means "no cursor", expressed as a predicate rather than two
        // separate query strings.
        auto r = db->execSqlSync(
            "SELECT n.id, n.kind, n.post_id, n.comment_id, n.read_at, n.created_at, "
            "       a.id AS actor_id, a.username AS actor_username, "
            "       a.profile_image AS actor_image, "
            "       p.title AS post_title, "
            "       c.content AS comment_content "
            "  FROM notifications n "
            "  LEFT JOIN users a    ON a.id = n.actor_id "
            "  LEFT JOIN posts p    ON p.id = n.post_id AND p.hidden_at IS NULL "
            "  LEFT JOIN comments c ON c.id = n.comment_id AND c.hidden_at IS NULL "
            " WHERE n.user_id = $1 AND ($3::bigint = 0 OR n.id < $3::bigint) "
            " ORDER BY n.id DESC "
            " LIMIT $2::int",
            userId, limit, beforeId);

        for (const auto& row : r) {
            Json::Value n;
            n["id"]         = row["id"].as<int64_t>();
            n["kind"]       = row["kind"].as<std::string>();
            n["created_at"] = row["created_at"].as<std::string>();
            n["read"]       = !row["read_at"].isNull();

            if (!row["actor_id"].isNull()) {
                n["actor"]["id"]       = row["actor_id"].as<int>();
                n["actor"]["username"] = row["actor_username"].as<std::string>();
                if (!row["actor_image"].isNull()) {
                    const auto img = row["actor_image"].as<std::string>();
                    if (!img.empty()) n["actor"]["profile_image"] = img;
                }
            }
            if (!row["post_id"].isNull())    n["post_id"]    = row["post_id"].as<int>();
            if (!row["comment_id"].isNull()) n["comment_id"] = row["comment_id"].as<int>();
            if (!row["post_title"].isNull()) n["post_title"] = row["post_title"].as<std::string>();

            if (!row["comment_content"].isNull()) {
                // A one-line preview. The full comment is one click away and
                // a notification list that reproduces entire comments stops
                // being scannable.
                auto text = row["comment_content"].as<std::string>();
                if (text.size() > 120) text = text.substr(0, 120) + "…";
                n["comment_preview"] = text;
            }
            out.append(n);
        }
    } catch (const DrogonDbException& e) {
        LOG_ERROR << "DB Error (notifications::list): " << e.base().what();
    }
    return out;
}

long long unreadCount(const DbClientPtr& db, int userId)
{
    try {
        auto r = db->execSqlSync(
            "SELECT count(*) AS n FROM notifications "
            " WHERE user_id = $1 AND read_at IS NULL",
            userId);
        return r.empty() ? 0 : r[0]["n"].as<long long>();
    } catch (const DrogonDbException& e) {
        LOG_ERROR << "DB Error (notifications::unreadCount): " << e.base().what();
        // Zero rather than an error: a wrong badge is better than a page
        // that will not render.
        return 0;
    }
}

bool markRead(const DbClientPtr& db, int userId, long long id)
{
    try {
        // Ownership is in the WHERE clause, not a separate check followed by
        // an update — there is then no window in which the row could belong
        // to someone else by the time the write lands, and no branch where
        // the check is forgotten.
        auto r = db->execSqlSync(
            "UPDATE notifications SET read_at = now() "
            " WHERE id = $2::bigint AND user_id = $1 AND read_at IS NULL "
            " RETURNING id",
            userId, id);
        return !r.empty();
    } catch (const DrogonDbException& e) {
        LOG_ERROR << "DB Error (notifications::markRead): " << e.base().what();
        return false;
    }
}

long long markAllRead(const DbClientPtr& db, int userId)
{
    try {
        auto r = db->execSqlSync(
            "WITH d AS ("
            "  UPDATE notifications SET read_at = now() "
            "   WHERE user_id = $1 AND read_at IS NULL RETURNING 1"
            ") SELECT count(*) AS n FROM d",
            userId);
        return r.empty() ? 0 : r[0]["n"].as<long long>();
    } catch (const DrogonDbException& e) {
        LOG_ERROR << "DB Error (notifications::markAllRead): " << e.base().what();
        return 0;
    }
}

} // namespace notifications
