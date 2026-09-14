#include "CommentController.h"
#include "../helpers/Notifications.h"
#include "../models/Comments.h"
#include "../models/Users.h"
#include "../helpers/HttpCache.h"
#include "../helpers/Security.h"
#include "../helpers/Workers.h"
#include <drogon/orm/Mapper.h>
#include <drogon/orm/Exception.h>
#include <trantor/utils/Logger.h>

#include <chrono>
#include <future>
#include <memory>

using namespace drogon;
using namespace drogon::orm;

namespace {
// Comments share their content with the pg_notify(blog_event) payload
// produced by trg_comments_notify; the trigger truncates to 200 bytes
// before json_build_object so it stays under PG's 8 KiB NOTIFY limit,
// but the stored content itself is bounded here so a single comment
// can't be a DoS vector on subsequent renders / list responses.
constexpr std::size_t kMaxCommentBytes = std::size_t{10} * 1024;
}

void CommentController::getPostComments(const HttpRequestPtr &req,
                                       std::function<void(const HttpResponsePtr &)> &&callback,
                                       int postId)
{
    // Raw SQL with an explicit LEFT JOIN replaces the Mapper-then-loop
    // pattern: one round-trip, no N+1, and updated_at (added in 0005)
    // is surfaced for ETag derivation without going through a model
    // regen. The LEFT JOIN preserves a comment whose author row was
    // deleted concurrently (CommentController previously caught that
    // case with a try/catch around findByPrimaryKey).
    static const char* kSql =
        "SELECT c.id, c.content, c.created_at, c.updated_at, c.parent_id, c.deleted_at, "
        "       u.id AS author_id, u.username AS author_username, "
        "       u.profile_image AS author_profile_image "
        "FROM comments c "
        // The thread inherits the post's visibility. Filtering only on
        // c.hidden_at made hiding a post a half-measure: the post left every
        // feed while GET /posts/{id}/comments kept serving its discussion to
        // anonymous callers, so moderating a thread removed the thing being
        // discussed and left the discussion. An INNER JOIN also means a
        // comment can never outlive the visibility of what it replies to.
        //
        // Deliberately not viewer-dependent: the response is ETagged and
        // cacheable, and making the row set depend on the session would
        // require Vary: Cookie on a hot public path. Drafts have no comments
        // to show anyway — createComment refuses them below.
        "JOIN posts p ON p.id = c.post_id "
        "            AND p.hidden_at IS NULL "
        "            AND p.published_at IS NOT NULL "
        "LEFT JOIN users u ON u.id = c.user_id "
        "WHERE c.hidden_at IS NULL AND c.post_id = $1 "
        // Ordered by id, not created_at: two comments posted in the same
        // millisecond would otherwise come back in an arbitrary order, and
        // the client nests by parent_id assuming a parent is seen before
        // its replies. id is monotonic, so that holds.
        "ORDER BY c.id ASC";

    auto dbClient = drogon::app().getDbClient();
    dbClient->execSqlAsync(
        kSql,
        [callback, req, postId](const Result& r) {
            // ETag from (post_id, count, max(updated_at)). Edits bump
            // updated_at via trg_comments_updated_at; inserts bump the
            // count and update max; deletes bump the count and may
            // lower the max (still a distinct fragment). Keying on
            // post_id keeps two posts' comment-list ETags disjoint
            // even when they coincidentally produce the same count
            // and max timestamp.
            std::int64_t maxTs = 0;
            for (const auto& row : r) {
                const auto ts = http_cache::parseTimestampMicros(
                                    row["updated_at"].as<std::string>());
                if (ts > maxTs) maxTs = ts;
            }
            const std::string etag = http_cache::makeWeakEtag({
                "comments",
                std::to_string(postId),
                std::to_string(static_cast<int>(r.size())),
                std::to_string(maxTs),
            });
            if (http_cache::ifNoneMatchHit(req, etag)) {
                callback(http_cache::makeNotModified(etag));
                return;
            }

            Json::Value ret;
            ret["comments"] = Json::Value(Json::arrayValue);
            for (const auto& row : r) {
                Json::Value commentJson;
                commentJson["id"] = row["id"].as<int>();
                commentJson["content"] = row["content"].as<std::string>();
                commentJson["created_at"] = row["created_at"].as<std::string>();
                // Null for a top-level comment. The client builds the tree;
                // sending a nested structure would make the ETag depend on
                // the shape rather than the contents.
                commentJson["parent_id"] = row["parent_id"].isNull()
                    ? Json::nullValue
                    : Json::Value(row["parent_id"].as<int>());

                // A tombstone (see deleteComment) keeps its place so the
                // replies under it still have a parent, but names nobody:
                // its author asked for it to be gone.
                const bool tombstoned = !row["deleted_at"].isNull();
                if (tombstoned) commentJson["deleted"] = true;
                if (!tombstoned && !row["author_id"].isNull()) {
                    commentJson["author"]["id"] = row["author_id"].as<int>();
                    commentJson["author"]["username"] = row["author_username"].as<std::string>();
                    if (!row["author_profile_image"].isNull()) {
                        auto img = row["author_profile_image"].as<std::string>();
                        if (!img.empty()) commentJson["author"]["profile_image"] = img;
                    }
                }
                ret["comments"].append(commentJson);
            }

            auto resp = HttpResponse::newHttpJsonResponse(ret);
            http_cache::applyCacheHeaders(resp, etag);
            callback(resp);
        },
        [callback](const DrogonDbException& e) {
            LOG_ERROR << "DB Error (comments): " << e.base().what();
            Json::Value ret;
            ret["error"] = "Failed to fetch comments";
            auto resp = HttpResponse::newHttpJsonResponse(ret);
            resp->setStatusCode(k500InternalServerError);
            callback(resp);
        },
        postId);
}

void CommentController::createComment(const HttpRequestPtr &req,
                                     std::function<void(const HttpResponsePtr &)> &&callback,
                                     int postId)
{
    auto session = req->session();
    auto userIdOpt = session->getOptional<int>("user_id");

    if (!userIdOpt.has_value()) {
        Json::Value ret;
        ret["error"] = "Not authenticated";
        auto resp = HttpResponse::newHttpJsonResponse(ret);
        resp->setStatusCode(k401Unauthorized);
        callback(resp);
        return;
    }

    // Per-user comment cap: 20 burst, 20/min — bounds comment spam.
    if (auto rl = security::rateLimitOr429(
            "comment_create", "uid:" + std::to_string(userIdOpt.value()),
            20.0, 20.0 / 60.0)) {
        callback(rl);
        return;
    }

    auto json = req->getJsonObject();
    if (!json) {
        auto resp = HttpResponse::newHttpJsonResponse(
            Json::Value("error: Invalid JSON"));
        resp->setStatusCode(k400BadRequest);
        callback(resp);
        return;
    }

    std::string content = (*json)["content"].asString();

    if (content.empty()) {
        Json::Value ret;
        ret["error"] = "Content is required";
        auto resp = HttpResponse::newHttpJsonResponse(ret);
        resp->setStatusCode(k400BadRequest);
        callback(resp);
        return;
    }
    if (content.size() > kMaxCommentBytes) {
        Json::Value ret;
        ret["error"] = "Comment too long";
        auto resp = HttpResponse::newHttpJsonResponse(ret);
        resp->setStatusCode(k413RequestEntityTooLarge);
        callback(resp);
        return;
    }

    // Optional: a reply rather than a top-level comment.
    const int parentId = (*json)["parent_id"].isInt() ? (*json)["parent_id"].asInt() : 0;

    // Up to four synchronous queries (post lookup, parent lookup, insert, one
    // or two notification inserts) — see helpers/Workers.h for why none of
    // them may run on an event loop. Posting a comment was parking one of the
    // twelve IO loops for the duration of all of them.
    const int userId = userIdOpt.value();
    workers::offload(workers::Pool::Auth, callback,
        [userId, postId, parentId, content, callback] {
    auto dbClient = drogon::app().getDbClient();

    try {
        // Confirm the post exists up front so commenting on a missing/deleted
        // post returns a clean 404 instead of letting the FK constraint
        // surface as a 500.
        // Hidden posts are 404 everywhere, including as a comment target —
        // otherwise a moderated thread keeps accepting replies.
        //
        // Drafts are 404 here too. They were not, and the gap was an
        // existence oracle: 201 rather than 404 told an unauthenticated
        // guesser that post N exists and is somebody's unpublished draft,
        // and the comment it planted then sat on a page its author had not
        // published. A draft is not a place anyone else can write.
        auto postRow = dbClient->execSqlSync(
            "SELECT user_id FROM posts "
            " WHERE id = $1 AND hidden_at IS NULL AND published_at IS NOT NULL",
            postId);
        if (postRow.empty()) {
            Json::Value ret;
            ret["error"] = "Post not found";
            auto resp = HttpResponse::newHttpJsonResponse(ret);
            resp->setStatusCode(k404NotFound);
            callback(resp);
            return;
        }
        const int postAuthorId = postRow[0]["user_id"].as<int>();

        // A reply has to point at a visible comment on *this* post.
        // Without the post_id check a reply could be attached to a comment
        // on a different post, producing a thread that renders nowhere and
        // a notification pointing at the wrong page.
        int parentAuthorId = 0;
        if (parentId > 0) {
            auto parent = dbClient->execSqlSync(
                "SELECT user_id FROM comments "
                " WHERE id = $1 AND post_id = $2 AND hidden_at IS NULL "
                // A tombstone takes no new replies; its author would
                // otherwise be notified about a comment they deleted.
                "   AND deleted_at IS NULL",
                parentId, postId);
            if (parent.empty()) {
                Json::Value ret;
                ret["error"] = "Parent comment not found";
                auto resp = HttpResponse::newHttpJsonResponse(ret);
                resp->setStatusCode(k404NotFound);
                callback(resp);
                return;
            }
            parentAuthorId = parent[0]["user_id"].as<int>();
        }

        // Raw SQL rather than the ORM mapper: parent_id was added in 0014
        // and the generated model has no accessor for it.
        auto ins = dbClient->execSqlSync(
            "INSERT INTO comments (post_id, user_id, content, parent_id) "
            "VALUES ($1, $2, $3, NULLIF($4::int, 0)) "
            "RETURNING id, created_at",
            postId, userId, content, parentId);
        const int newId = ins[0]["id"].as<int>();

        // Notify, in order of who most wants to know.
        //
        // A reply notifies the comment's author; a top-level comment
        // notifies the post's author. When someone replies to their own
        // comment on someone else's post, both are relevant — and emit()
        // drops the self-notification, so the author of the reply never
        // hears about it either way.
        if (parentId > 0) {
            notifications::emit(dbClient, parentAuthorId, userId,
                                notifications::Kind::Reply, postId, newId);
            // Also tell the post's author, unless they are the one being
            // replied to (they would get two notifications for one event).
            if (postAuthorId != parentAuthorId) {
                notifications::emit(dbClient, postAuthorId, userId,
                                    notifications::Kind::Comment, postId, newId);
            }
        } else {
            notifications::emit(dbClient, postAuthorId, userId,
                                notifications::Kind::Comment, postId, newId);
        }

        Json::Value ret;
        ret["message"] = "Comment created successfully";
        ret["comment"]["id"]         = newId;
        ret["comment"]["content"]    = content;
        ret["comment"]["created_at"] = ins[0]["created_at"].as<std::string>();
        ret["comment"]["parent_id"]  = parentId > 0 ? Json::Value(parentId)
                                                    : Json::nullValue;

        auto resp = HttpResponse::newHttpJsonResponse(ret);
        resp->setStatusCode(k201Created);
        callback(resp);
    } catch (const DrogonDbException &e) {
        LOG_ERROR << "DB Error: " << e.base().what();
        Json::Value ret;
        ret["error"] = "Failed to create comment";
        auto resp = HttpResponse::newHttpJsonResponse(ret);
        resp->setStatusCode(k500InternalServerError);
        callback(resp);
    }
        });
}

namespace {
HttpResponsePtr commentError(HttpStatusCode code, const std::string& msg)
{
    Json::Value ret;
    ret["error"] = msg;
    auto resp = HttpResponse::newHttpJsonResponse(ret);
    resp->setStatusCode(code);
    return resp;
}
} // namespace

void CommentController::updateComment(const HttpRequestPtr &req,
                                     std::function<void(const HttpResponsePtr &)> &&callback,
                                     int commentId)
{
    auto session = req->session();
    auto userIdOpt = session->getOptional<int>("user_id");

    if (!userIdOpt.has_value()) {
        callback(commentError(k401Unauthorized, "Not authenticated"));
        return;
    }

    auto json = req->getJsonObject();
    if (!json) {
        auto resp = HttpResponse::newHttpJsonResponse(
            Json::Value("error: Invalid JSON"));
        resp->setStatusCode(k400BadRequest);
        callback(resp);
        return;
    }

    // Same rules as createComment. An edit used to accept "" and blank the
    // comment out — a deletion that skipped the tombstone logic below.
    const std::string newContent = (*json)["content"].asString();
    if (newContent.empty()) {
        callback(commentError(k400BadRequest, "Content is required"));
        return;
    }
    if (newContent.size() > kMaxCommentBytes) {
        callback(commentError(k413RequestEntityTooLarge, "Comment too long"));
        return;
    }

    const int userId = userIdOpt.value();
    workers::offload(workers::Pool::Auth, callback,
        [userId, commentId, newContent, callback] {
            try {
                auto db = drogon::app().getDbClient();
                // A tombstone is not there to edit: letting its author
                // rewrite "[deleted]" would quietly undo the deletion.
                const auto owner = db->execSqlSync(
                    "SELECT user_id FROM comments "
                    " WHERE id = $1 AND deleted_at IS NULL",
                    commentId);
                if (owner.empty()) {
                    callback(commentError(k404NotFound, "Comment not found"));
                    return;
                }
                if (owner[0]["user_id"].as<int>() != userId) {
                    callback(commentError(k403Forbidden, "Unauthorized"));
                    return;
                }
                // The guards are repeated here so a delete that lands
                // between the two statements cannot be edited back.
                const auto r = db->execSqlSync(
                    "UPDATE comments SET content = $3 "
                    " WHERE id = $1 AND user_id = $2 AND deleted_at IS NULL "
                    "RETURNING id, content",
                    commentId, userId, newContent);
                if (r.empty()) {
                    callback(commentError(k404NotFound, "Comment not found"));
                    return;
                }
                Json::Value ret;
                ret["message"] = "Comment updated successfully";
                ret["comment"]["id"] = r[0]["id"].as<int>();
                ret["comment"]["content"] = r[0]["content"].as<std::string>();
                callback(HttpResponse::newHttpJsonResponse(ret));
            } catch (const DrogonDbException &e) {
                LOG_ERROR << "DB Error (update comment): " << e.base().what();
                callback(commentError(k500InternalServerError, "Failed to update comment"));
            }
        });
}

void CommentController::deleteComment(const HttpRequestPtr &req,
                                     std::function<void(const HttpResponsePtr &)> &&callback,
                                     int commentId)
{
    auto session = req->session();
    auto userIdOpt = session->getOptional<int>("user_id");

    if (!userIdOpt.has_value()) {
        callback(commentError(k401Unauthorized, "Not authenticated"));
        return;
    }

    // comments.parent_id is ON DELETE CASCADE, so deleting a comment that
    // has replies deletes the replies too — other people's words, gone
    // because someone removed theirs. Such a comment is tombstoned instead
    // (content "[deleted]", deleted_at set), the way account erasure has
    // always done it; one without replies is deleted outright.
    //
    // Choosing between the two is a race unless the row is locked first. A
    // reply's INSERT takes FOR KEY SHARE on its parent for the foreign-key
    // check, which FOR UPDATE blocks, so once the lock is held no reply can
    // land; and the second statement's snapshot is taken after the lock, so
    // it sees every reply that committed before it. One statement would
    // decide on a snapshot from before the lock and could still cascade a
    // reply committed in between.
    const int userId = userIdOpt.value();
    workers::offload(workers::Pool::Auth, callback,
        [userId, commentId, callback] {
            // newTransaction() pins one connection, and the COMMIT's outcome
            // arrives through this callback — see AuthControllerPrivacy.cc.
            auto client = drogon::app().getDbClient();
            auto committed = std::make_shared<std::promise<bool>>();
            auto commitResult = committed->get_future();
            auto db = client->newTransaction(
                [committed](bool ok) { committed->set_value(ok); });

            HttpResponsePtr refusal;
            bool tombstoned = false;
            try {
                const auto row = db->execSqlSync(
                    "SELECT user_id FROM comments "
                    " WHERE id = $1 AND deleted_at IS NULL FOR UPDATE",
                    commentId);
                if (row.empty()) {
                    refusal = commentError(k404NotFound, "Comment not found");
                } else if (row[0]["user_id"].as<int>() != userId) {
                    refusal = commentError(k403Forbidden, "Unauthorized");
                } else {
                    // Every child row counts, hidden or tombstoned ones
                    // included: the cascade would take those too.
                    const auto r = db->execSqlSync(
                        "WITH t AS (SELECT EXISTS (SELECT 1 FROM comments "
                        "                           WHERE parent_id = $1) AS has_replies), "
                        "tomb AS (UPDATE comments "
                        "            SET content = '[deleted]', deleted_at = now() "
                        "          WHERE id = $1 AND (SELECT has_replies FROM t) "
                        "         RETURNING id), "
                        "gone AS (DELETE FROM comments "
                        "          WHERE id = $1 AND NOT (SELECT has_replies FROM t) "
                        "         RETURNING id) "
                        "SELECT (SELECT count(*) FROM tomb) AS tombstoned, "
                        "       (SELECT count(*) FROM gone) AS deleted",
                        commentId);
                    tombstoned = r[0]["tombstoned"].as<std::int64_t>() > 0;
                }
            } catch (const DrogonDbException &e) {
                LOG_ERROR << "DB Error (delete comment): " << e.base().what();
                refusal = commentError(k500InternalServerError, "Failed to delete comment");
            }

            // Nothing is written on a refusal; roll back so the destructor
            // releases the lock instead of committing.
            if (refusal) db->rollback();
            db.reset();
            if (refusal) {
                callback(refusal);
                return;
            }
            if (commitResult.wait_for(std::chrono::seconds(30)) !=
                    std::future_status::ready ||
                !commitResult.get())
            {
                LOG_ERROR << "comment delete commit failed: id=" << commentId;
                callback(commentError(k500InternalServerError, "Failed to delete comment"));
                return;
            }

            Json::Value ret;
            ret["message"] = tombstoned ? "Comment deleted; its replies were kept"
                                        : "Comment deleted successfully";
            ret["tombstoned"] = tombstoned;
            callback(HttpResponse::newHttpJsonResponse(ret));
        });
}
