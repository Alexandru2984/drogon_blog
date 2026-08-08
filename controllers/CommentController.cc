#include "CommentController.h"
#include "../helpers/Notifications.h"
#include "../models/Comments.h"
#include "../models/Users.h"
#include "../helpers/HttpCache.h"
#include "../helpers/Security.h"
#include <drogon/orm/Mapper.h>
#include <drogon/orm/Exception.h>
#include <trantor/utils/Logger.h>

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
        "SELECT c.id, c.content, c.created_at, c.updated_at, c.parent_id, "
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

                if (!row["author_id"].isNull()) {
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
                " WHERE id = $1 AND post_id = $2 AND hidden_at IS NULL",
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
            postId, userIdOpt.value(), content, parentId);
        const int newId = ins[0]["id"].as<int>();

        // Notify, in order of who most wants to know.
        //
        // A reply notifies the comment's author; a top-level comment
        // notifies the post's author. When someone replies to their own
        // comment on someone else's post, both are relevant — and emit()
        // drops the self-notification, so the author of the reply never
        // hears about it either way.
        if (parentId > 0) {
            notifications::emit(dbClient, parentAuthorId, userIdOpt.value(),
                                notifications::Kind::Reply, postId, newId);
            // Also tell the post's author, unless they are the one being
            // replied to (they would get two notifications for one event).
            if (postAuthorId != parentAuthorId) {
                notifications::emit(dbClient, postAuthorId, userIdOpt.value(),
                                    notifications::Kind::Comment, postId, newId);
            }
        } else {
            notifications::emit(dbClient, postAuthorId, userIdOpt.value(),
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
}

void CommentController::updateComment(const HttpRequestPtr &req,
                                     std::function<void(const HttpResponsePtr &)> &&callback,
                                     int commentId)
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

    auto json = req->getJsonObject();
    if (!json) {
        auto resp = HttpResponse::newHttpJsonResponse(
            Json::Value("error: Invalid JSON"));
        resp->setStatusCode(k400BadRequest);
        callback(resp);
        return;
    }

    auto dbClient = drogon::app().getDbClient();
    Mapper<drogon_model::blog_db::Comments> mapper(dbClient);

    try {
        auto comment = mapper.findByPrimaryKey(commentId);

        // Check if user owns the comment
        if (comment.getValueOfUserId() != userIdOpt.value()) {
            Json::Value ret;
            ret["error"] = "Unauthorized";
            auto resp = HttpResponse::newHttpJsonResponse(ret);
            resp->setStatusCode(k403Forbidden);
            callback(resp);
            return;
        }

        if (json->isMember("content")) {
            const std::string newContent = (*json)["content"].asString();
            if (newContent.size() > kMaxCommentBytes) {
                Json::Value ret;
                ret["error"] = "Comment too long";
                auto resp = HttpResponse::newHttpJsonResponse(ret);
                resp->setStatusCode(k413RequestEntityTooLarge);
                callback(resp);
                return;
            }
            comment.setContent(newContent);
        }

        mapper.update(comment);

        Json::Value ret;
        ret["message"] = "Comment updated successfully";
        ret["comment"]["id"] = comment.getValueOfId();
        ret["comment"]["content"] = comment.getValueOfContent();

        auto resp = HttpResponse::newHttpJsonResponse(ret);
        callback(resp);
    } catch (const UnexpectedRows &) {
        Json::Value ret;
        ret["error"] = "Comment not found";
        auto resp = HttpResponse::newHttpJsonResponse(ret);
        resp->setStatusCode(k404NotFound);
        callback(resp);
    } catch (const DrogonDbException &e) {
        LOG_ERROR << "DB Error: " << e.base().what();
        Json::Value ret;
        ret["error"] = "Failed to update comment";
        auto resp = HttpResponse::newHttpJsonResponse(ret);
        resp->setStatusCode(k500InternalServerError);
        callback(resp);
    }
}

void CommentController::deleteComment(const HttpRequestPtr &req,
                                     std::function<void(const HttpResponsePtr &)> &&callback,
                                     int commentId)
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

    auto dbClient = drogon::app().getDbClient();
    Mapper<drogon_model::blog_db::Comments> mapper(dbClient);

    try {
        auto comment = mapper.findByPrimaryKey(commentId);

        // Check if user owns the comment
        if (comment.getValueOfUserId() != userIdOpt.value()) {
            Json::Value ret;
            ret["error"] = "Unauthorized";
            auto resp = HttpResponse::newHttpJsonResponse(ret);
            resp->setStatusCode(k403Forbidden);
            callback(resp);
            return;
        }

        mapper.deleteByPrimaryKey(commentId);

        Json::Value ret;
        ret["message"] = "Comment deleted successfully";
        auto resp = HttpResponse::newHttpJsonResponse(ret);
        callback(resp);
    } catch (const UnexpectedRows &) {
        Json::Value ret;
        ret["error"] = "Comment not found";
        auto resp = HttpResponse::newHttpJsonResponse(ret);
        resp->setStatusCode(k404NotFound);
        callback(resp);
    } catch (const DrogonDbException &e) {
        LOG_ERROR << "DB Error: " << e.base().what();
        Json::Value ret;
        ret["error"] = "Failed to delete comment";
        auto resp = HttpResponse::newHttpJsonResponse(ret);
        resp->setStatusCode(k500InternalServerError);
        callback(resp);
    }
}
