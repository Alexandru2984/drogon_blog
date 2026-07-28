#include "CommentController.h"
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
        "SELECT c.id, c.content, c.created_at, c.updated_at, "
        "       u.id AS author_id, u.username AS author_username, "
        "       u.profile_image AS author_profile_image "
        "FROM comments c "
        "LEFT JOIN users u ON u.id = c.user_id "
        "WHERE c.hidden_at IS NULL AND c.post_id = $1 "
        "ORDER BY c.created_at ASC";

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

    auto dbClient = drogon::app().getDbClient();
    Mapper<drogon_model::blog_db::Comments> mapper(dbClient);

    drogon_model::blog_db::Comments newComment;
    newComment.setPostId(postId);
    newComment.setUserId(userIdOpt.value());
    newComment.setContent(content);

    try {
        // Confirm the post exists up front so commenting on a missing/deleted
        // post returns a clean 404 instead of letting the FK constraint
        // surface as a 500.
        // Hidden posts are 404 everywhere, including as a comment target —
        // otherwise a moderated thread keeps accepting replies.
        if (dbClient->execSqlSync(
                "SELECT 1 FROM posts WHERE id = $1 AND hidden_at IS NULL",
                postId).empty()) {
            Json::Value ret;
            ret["error"] = "Post not found";
            auto resp = HttpResponse::newHttpJsonResponse(ret);
            resp->setStatusCode(k404NotFound);
            callback(resp);
            return;
        }
        mapper.insert(newComment);

        Json::Value ret;
        ret["message"] = "Comment created successfully";
        ret["comment"]["id"] = newComment.getValueOfId();
        ret["comment"]["content"] = newComment.getValueOfContent();
        ret["comment"]["created_at"] = newComment.getValueOfCreatedAt().toDbStringLocal();

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
