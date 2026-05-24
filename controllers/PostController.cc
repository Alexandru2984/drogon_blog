#include "PostController.h"
#include "../models/Posts.h"
#include "../models/Users.h"
#include "../models/Likes.h"
#include "../helpers/AuditLog.h"
#include "../helpers/HttpCache.h"
#include "../helpers/Markdown.h"
#include <drogon/orm/Mapper.h>
#include <drogon/orm/Exception.h>
#include <trantor/utils/Logger.h>

#include <algorithm>
#include <string>

using namespace drogon;
using namespace drogon::orm;

namespace {

constexpr int kDefaultPageSize = 20;
constexpr int kMaxPageSize     = 50;

int clampLimit(const std::string& raw)
{
    if (raw.empty()) return kDefaultPageSize;
    try {
        int v = std::stoi(raw);
        if (v < 1) return kDefaultPageSize;
        return std::min(v, kMaxPageSize);
    } catch (...) { return kDefaultPageSize; }
}

int parseCursor(const std::string& raw)
{
    if (raw.empty()) return 0;
    try {
        int v = std::stoi(raw);
        return v > 0 ? v : 0;
    } catch (...) { return 0; }
}

} // namespace

void PostController::getAllPosts(const HttpRequestPtr &req,
                                std::function<void(const HttpResponsePtr &)> &&callback)
{
    auto dbClient = drogon::app().getDbClient();

    // Cursor-based pagination. `before` is the id of the oldest post the
    // client already has — we return rows strictly older than it. IDs are
    // monotonic (IDENTITY) so ordering by id DESC matches created_at DESC
    // and gives a stable cursor.
    const int  limit  = clampLimit(req->getParameter("limit"));
    const int  cursor = parseCursor(req->getParameter("before"));

    // cppcheck-suppress variableScope ; kept at function scope so both
    // branches resolve to the same string literal.
    static const char* kSqlWithCursor =
        "SELECT p.id, p.title, p.content, p.content_html, p.created_at, p.updated_at, "
        "       u.id AS author_id, u.username AS author_username, u.profile_image AS author_profile_image "
        "FROM posts p "
        "LEFT JOIN users u ON u.id = p.user_id "
        "WHERE p.id < $1 "
        "ORDER BY p.id DESC "
        "LIMIT $2";
    // cppcheck-suppress variableScope ; ditto
    static const char* kSqlFirstPage =
        "SELECT p.id, p.title, p.content, p.content_html, p.created_at, p.updated_at, "
        "       u.id AS author_id, u.username AS author_username, u.profile_image AS author_profile_image "
        "FROM posts p "
        "LEFT JOIN users u ON u.id = p.user_id "
        "ORDER BY p.id DESC "
        "LIMIT $1";

    auto onOk = [callback, req, limit, cursor](const Result& r) {
        // ETag from the page contents: max(updated_at) inside this page
        // + row count + the pagination keys that defined the page. Any
        // INSERT/UPDATE that lands inside the cursor window will bump
        // max(updated_at); rows leaving / entering due to creation will
        // bump the count or the max id; cache-bypassing parameter
        // changes get distinct tags. We don't include the JOIN'd author
        // fields — those don't change without bumping posts.updated_at
        // because the JOIN is read-only.
        std::int64_t maxTs = 0;
        int64_t      maxId = 0;
        int64_t      minId = 0;
        for (const auto& row : r) {
            const auto id  = row["id"].as<int64_t>();
            const auto ts  = http_cache::parseTimestampMicros(
                                 row["updated_at"].as<std::string>());
            if (ts > maxTs)   maxTs = ts;
            if (id > maxId)   maxId = id;
            if (minId == 0 || id < minId) minId = id;
        }
        const std::string etag = http_cache::makeWeakEtag({
            "posts",
            std::to_string(maxTs),
            std::to_string(maxId),
            std::to_string(static_cast<int>(r.size())),
            std::to_string(cursor),
            std::to_string(limit),
        });
        if (http_cache::ifNoneMatchHit(req, etag)) {
            callback(http_cache::makeNotModified(etag));
            return;
        }

        Json::Value ret;
        ret["posts"] = Json::Value(Json::arrayValue);

        for (const auto& row : r) {
            Json::Value post;
            const auto id      = row["id"].as<int64_t>();
            post["id"]         = id;
            post["title"]      = row["title"].as<std::string>();
            post["content"]    = row["content"].as<std::string>();
            if (!row["content_html"].isNull())
                post["content_html"] = row["content_html"].as<std::string>();
            post["created_at"] = row["created_at"].as<std::string>();
            post["updated_at"] = row["updated_at"].as<std::string>();

            if (!row["author_id"].isNull()) {
                post["author"]["id"]       = row["author_id"].as<int64_t>();
                post["author"]["username"] = row["author_username"].as<std::string>();
                if (!row["author_profile_image"].isNull()) {
                    auto img = row["author_profile_image"].as<std::string>();
                    if (!img.empty()) post["author"]["profile_image"] = img;
                }
            }
            ret["posts"].append(post);
        }

        // Only emit a cursor when this page filled the limit; otherwise the
        // client knows there's nothing more to fetch.
        if (static_cast<int>(r.size()) == limit && minId > 0) {
            ret["next_cursor"] = static_cast<Json::Int64>(minId);
        } else {
            ret["next_cursor"] = Json::nullValue;
        }

        auto resp = HttpResponse::newHttpJsonResponse(ret);
        http_cache::applyCacheHeaders(resp, etag);
        callback(resp);
    };
    auto onErr = [callback](const DrogonDbException& e) {
        LOG_ERROR << "DB Error (getAllPosts): " << e.base().what();
        Json::Value ret;
        ret["error"] = "Failed to fetch posts";
        auto resp = HttpResponse::newHttpJsonResponse(ret);
        resp->setStatusCode(k500InternalServerError);
        callback(resp);
    };

    // PG infers parameter types from the prepared statement context: cursor
    // is compared against posts.id (int4) so it must bind as int32; LIMIT is
    // bigint internally so it must bind as int64. Mixing them up triggers
    // "insufficient data left in message" at parse time.
    const int64_t limit64 = limit;
    if (cursor > 0) {
        dbClient->execSqlAsync(kSqlWithCursor, onOk, onErr, cursor, limit64);
    } else {
        dbClient->execSqlAsync(kSqlFirstPage,  onOk, onErr, limit64);
    }
}

void PostController::searchPosts(const HttpRequestPtr &req,
                                std::function<void(const HttpResponsePtr &)> &&callback)
{
    const std::string q = req->getParameter("q");
    if (q.empty() || q.size() > 256) {
        Json::Value ret;
        ret["error"] = "Missing or oversized query parameter `q`";
        auto resp = HttpResponse::newHttpJsonResponse(ret);
        resp->setStatusCode(k400BadRequest);
        callback(resp);
        return;
    }

    // websearch_to_tsquery tolerates raw user input (quoted phrases, OR, -negate)
    // without throwing on punctuation. ts_headline returns highlighted snippets;
    // ts_rank orders by relevance, falling back to recency as a tiebreaker.
    static const char* kSql =
        "WITH q AS (SELECT websearch_to_tsquery('english', $1) AS query) "
        "SELECT p.id, p.title, "
        "       ts_headline('english', p.content, q.query, "
        "                   'MaxFragments=2,MaxWords=24,MinWords=8,"
        "ShortWord=2,StartSel=<mark>,StopSel=</mark>') AS snippet, "
        "       p.created_at, p.updated_at, "
        "       u.id AS author_id, u.username AS author_username, "
        "       u.profile_image AS author_profile_image, "
        "       ts_rank(p.search, q.query) AS rank "
        "FROM posts p "
        "CROSS JOIN q "
        "LEFT JOIN users u ON u.id = p.user_id "
        "WHERE p.search @@ q.query "
        "ORDER BY rank DESC, p.created_at DESC "
        "LIMIT 50";

    auto dbClient = drogon::app().getDbClient();
    dbClient->execSqlAsync(
        kSql,
        [callback, req, q](const Result& r) {
            // ETag from (q, max(updated_at over matches), count). Adding
            // or editing a matching post changes one of those. Editing
            // a non-matching post outside the result set has no effect,
            // which is correct.
            std::int64_t maxTs = 0;
            for (const auto& row : r) {
                const auto ts = http_cache::parseTimestampMicros(
                                    row["updated_at"].as<std::string>());
                if (ts > maxTs) maxTs = ts;
            }
            const std::string etag = http_cache::makeWeakEtag({
                "search", q,
                std::to_string(maxTs),
                std::to_string(static_cast<int>(r.size())),
            });
            if (http_cache::ifNoneMatchHit(req, etag)) {
                callback(http_cache::makeNotModified(etag));
                return;
            }

            Json::Value ret;
            ret["query"] = q;
            ret["count"] = static_cast<Json::UInt>(r.size());
            ret["posts"] = Json::Value(Json::arrayValue);

            for (const auto& row : r) {
                Json::Value post;
                post["id"]         = row["id"].as<int64_t>();
                post["title"]      = row["title"].as<std::string>();
                post["snippet"]    = row["snippet"].as<std::string>();
                post["created_at"] = row["created_at"].as<std::string>();
                post["updated_at"] = row["updated_at"].as<std::string>();
                post["rank"]       = row["rank"].as<double>();

                if (!row["author_id"].isNull()) {
                    post["author"]["id"]       = row["author_id"].as<int64_t>();
                    post["author"]["username"] = row["author_username"].as<std::string>();
                    if (!row["author_profile_image"].isNull()) {
                        auto img = row["author_profile_image"].as<std::string>();
                        if (!img.empty()) post["author"]["profile_image"] = img;
                    }
                }
                ret["posts"].append(post);
            }
            auto resp = HttpResponse::newHttpJsonResponse(ret);
            http_cache::applyCacheHeaders(resp, etag);
            callback(resp);
        },
        [callback](const DrogonDbException& e) {
            LOG_ERROR << "DB Error (searchPosts): " << e.base().what();
            Json::Value ret;
            ret["error"] = "Search failed";
            auto resp = HttpResponse::newHttpJsonResponse(ret);
            resp->setStatusCode(k500InternalServerError);
            callback(resp);
        },
        q);
}

void PostController::getPost(const HttpRequestPtr &req,
                            std::function<void(const HttpResponsePtr &)> &&callback,
                            int postId)
{
    auto dbClient = drogon::app().getDbClient();

    static const char* kSql =
        "SELECT p.id, p.title, p.content, p.content_html, p.created_at, p.updated_at, "
        "       u.id AS author_id, u.username AS author_username, u.profile_image AS author_profile_image "
        "FROM posts p "
        "LEFT JOIN users u ON u.id = p.user_id "
        "WHERE p.id = $1";

    dbClient->execSqlAsync(
        kSql,
        [callback, req, postId](const Result& r) {
            if (r.empty()) {
                Json::Value ret;
                ret["error"] = "Post not found";
                auto resp = HttpResponse::newHttpJsonResponse(ret);
                resp->setStatusCode(k404NotFound);
                callback(resp);
                return;
            }
            const auto& row = r[0];

            // ETag derives from (id, updated_at). Anything that bumps
            // updated_at (UPDATE trigger fires on every row write) gives
            // the resource a new tag; comments/likes don't.
            const std::string updatedAt = row["updated_at"].as<std::string>();
            const std::string etag = http_cache::makeWeakEtag({
                "post", std::to_string(postId),
                std::to_string(http_cache::parseTimestampMicros(updatedAt)),
            });
            if (http_cache::ifNoneMatchHit(req, etag)) {
                callback(http_cache::makeNotModified(etag));
                return;
            }

            Json::Value ret;
            ret["id"]         = row["id"].as<int64_t>();
            ret["title"]      = row["title"].as<std::string>();
            ret["content"]    = row["content"].as<std::string>();
            if (!row["content_html"].isNull())
                ret["content_html"] = row["content_html"].as<std::string>();
            ret["created_at"] = row["created_at"].as<std::string>();
            ret["updated_at"] = updatedAt;

            if (!row["author_id"].isNull()) {
                ret["author"]["id"]       = row["author_id"].as<int64_t>();
                ret["author"]["username"] = row["author_username"].as<std::string>();
                if (!row["author_profile_image"].isNull()) {
                    auto img = row["author_profile_image"].as<std::string>();
                    if (!img.empty()) ret["author"]["profile_image"] = img;
                }
            }
            auto resp = HttpResponse::newHttpJsonResponse(ret);
            http_cache::applyCacheHeaders(resp, etag);
            callback(resp);
        },
        [callback](const DrogonDbException& e) {
            LOG_ERROR << "DB Error (getPost): " << e.base().what();
            Json::Value ret;
            ret["error"] = "Post not found";
            auto resp = HttpResponse::newHttpJsonResponse(ret);
            resp->setStatusCode(k404NotFound);
            callback(resp);
        },
        postId);
}

void PostController::createPost(const HttpRequestPtr &req,
                               std::function<void(const HttpResponsePtr &)> &&callback)
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

    std::string title = (*json)["title"].asString();
    std::string content = (*json)["content"].asString();

    if (title.empty() || content.empty()) {
        Json::Value ret;
        ret["error"] = "Title and content are required";
        auto resp = HttpResponse::newHttpJsonResponse(ret);
        resp->setStatusCode(k400BadRequest);
        callback(resp);
        return;
    }

    auto dbClient = drogon::app().getDbClient();
    Mapper<drogon_model::blog_db::Posts> mapper(dbClient);

    // Render markdown once at write-time and store both the raw source (so
    // we can re-render if rendering policy changes) and the resulting safe
    // HTML (so reads stay cheap).
    const std::string contentHtml = markdown::renderToSafeHtml(content);

    drogon_model::blog_db::Posts newPost;
    newPost.setUserId(userIdOpt.value());
    newPost.setTitle(title);
    newPost.setContent(content);
    newPost.setContentHtml(contentHtml);

    try {
        mapper.insert(newPost);

        Json::Value ret;
        ret["message"] = "Post created successfully";
        ret["post"]["id"]           = newPost.getValueOfId();
        ret["post"]["title"]        = newPost.getValueOfTitle();
        ret["post"]["content"]      = newPost.getValueOfContent();
        ret["post"]["content_html"] = newPost.getValueOfContentHtml();

        auto resp = HttpResponse::newHttpJsonResponse(ret);
        resp->setStatusCode(k201Created);
        callback(resp);
    } catch (const DrogonDbException &e) {
        LOG_ERROR << "DB Error: " << e.base().what();
        Json::Value ret;
        ret["error"] = "Failed to create post";
        auto resp = HttpResponse::newHttpJsonResponse(ret);
        resp->setStatusCode(k500InternalServerError);
        callback(resp);
    }
}

void PostController::updatePost(const HttpRequestPtr &req,
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

    auto json = req->getJsonObject();
    if (!json) {
        auto resp = HttpResponse::newHttpJsonResponse(
            Json::Value("error: Invalid JSON"));
        resp->setStatusCode(k400BadRequest);
        callback(resp);
        return;
    }

    auto dbClient = drogon::app().getDbClient();
    Mapper<drogon_model::blog_db::Posts> mapper(dbClient);

    try {
        auto post = mapper.findByPrimaryKey(postId);

        // Check if user owns the post
        if (post.getValueOfUserId() != userIdOpt.value()) {
            Json::Value ret;
            ret["error"] = "Unauthorized";
            auto resp = HttpResponse::newHttpJsonResponse(ret);
            resp->setStatusCode(k403Forbidden);
            callback(resp);
            return;
        }

        if (json->isMember("title")) {
            post.setTitle((*json)["title"].asString());
        }
        if (json->isMember("content")) {
            const std::string newContent = (*json)["content"].asString();
            post.setContent(newContent);
            post.setContentHtml(markdown::renderToSafeHtml(newContent));
        }

        mapper.update(post);

        Json::Value ret;
        ret["message"] = "Post updated successfully";
        ret["post"]["id"] = post.getValueOfId();
        ret["post"]["title"] = post.getValueOfTitle();
        ret["post"]["content"] = post.getValueOfContent();

        auto resp = HttpResponse::newHttpJsonResponse(ret);
        callback(resp);
    } catch (const UnexpectedRows &) {
        Json::Value ret;
        ret["error"] = "Post not found";
        auto resp = HttpResponse::newHttpJsonResponse(ret);
        resp->setStatusCode(k404NotFound);
        callback(resp);
    } catch (const DrogonDbException &e) {
        LOG_ERROR << "DB Error: " << e.base().what();
        Json::Value ret;
        ret["error"] = "Failed to update post";
        auto resp = HttpResponse::newHttpJsonResponse(ret);
        resp->setStatusCode(k500InternalServerError);
        callback(resp);
    }
}

void PostController::deletePost(const HttpRequestPtr &req,
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

    auto dbClient = drogon::app().getDbClient();
    Mapper<drogon_model::blog_db::Posts> mapper(dbClient);

    try {
        auto post = mapper.findByPrimaryKey(postId);

        // Check if user owns the post
        if (post.getValueOfUserId() != userIdOpt.value()) {
            Json::Value ret;
            ret["error"] = "Unauthorized";
            auto resp = HttpResponse::newHttpJsonResponse(ret);
            resp->setStatusCode(k403Forbidden);
            callback(resp);
            return;
        }

        mapper.deleteByPrimaryKey(postId);

        Json::Value meta;
        meta["title"] = post.getValueOfTitle();
        audit_log::record(req, {"post.delete", userIdOpt,
                                std::string{"post"},
                                static_cast<std::int64_t>(postId),
                                std::move(meta)});

        Json::Value ret;
        ret["message"] = "Post deleted successfully";
        auto resp = HttpResponse::newHttpJsonResponse(ret);
        callback(resp);
    } catch (const UnexpectedRows &) {
        Json::Value ret;
        ret["error"] = "Post not found";
        auto resp = HttpResponse::newHttpJsonResponse(ret);
        resp->setStatusCode(k404NotFound);
        callback(resp);
    } catch (const DrogonDbException &e) {
        LOG_ERROR << "DB Error: " << e.base().what();
        Json::Value ret;
        ret["error"] = "Failed to delete post";
        auto resp = HttpResponse::newHttpJsonResponse(ret);
        resp->setStatusCode(k500InternalServerError);
        callback(resp);
    }
}

void PostController::getUserPosts(const HttpRequestPtr &req,
                                 std::function<void(const HttpResponsePtr &)> &&callback,
                                 int userId)
{
    auto dbClient = drogon::app().getDbClient();
    Mapper<drogon_model::blog_db::Posts> mapper(dbClient);

    try {
        auto posts = mapper.findBy(
            Criteria(drogon_model::blog_db::Posts::Cols::_user_id,
                    CompareOperator::EQ, userId)
        );

        // ETag tracks (user_id, count, max(updated_at)). Adding /
        // removing / editing one of this user's posts changes one of
        // those three; other users' posts don't affect it.
        std::int64_t maxTs = 0;
        for (const auto& p : posts) {
            const auto ts = http_cache::parseTimestampMicros(
                                p.getValueOfUpdatedAt().toDbStringLocal());
            if (ts > maxTs) maxTs = ts;
        }
        const std::string etag = http_cache::makeWeakEtag({
            "user-posts", std::to_string(userId),
            std::to_string(maxTs),
            std::to_string(static_cast<int>(posts.size())),
        });
        if (http_cache::ifNoneMatchHit(req, etag)) {
            callback(http_cache::makeNotModified(etag));
            return;
        }

        Json::Value ret;
        ret["posts"] = Json::Value(Json::arrayValue);

        for (const auto &post : posts) {
            Json::Value postJson;
            postJson["id"] = post.getValueOfId();
            postJson["title"] = post.getValueOfTitle();
            postJson["content"] = post.getValueOfContent();
            postJson["created_at"] = post.getValueOfCreatedAt().toDbStringLocal();
            ret["posts"].append(postJson);
        }

        auto resp = HttpResponse::newHttpJsonResponse(ret);
        http_cache::applyCacheHeaders(resp, etag);
        callback(resp);
    } catch (const DrogonDbException &e) {
        LOG_ERROR << "DB Error: " << e.base().what();
        Json::Value ret;
        ret["error"] = "Failed to fetch posts";
        auto resp = HttpResponse::newHttpJsonResponse(ret);
        resp->setStatusCode(k500InternalServerError);
        callback(resp);
    }
}

void PostController::likePost(const HttpRequestPtr &req,
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

    auto dbClient = drogon::app().getDbClient();
    Mapper<drogon_model::blog_db::Likes> mapper(dbClient);

    try {
        // Check if already liked
        auto existingLikes = mapper.findBy(
            Criteria(drogon_model::blog_db::Likes::Cols::_post_id, 
                    CompareOperator::EQ, postId) &&
            Criteria(drogon_model::blog_db::Likes::Cols::_user_id, 
                    CompareOperator::EQ, userIdOpt.value())
        );

        if (existingLikes.size() > 0) {
            Json::Value ret;
            ret["error"] = "Post already liked";
            auto resp = HttpResponse::newHttpJsonResponse(ret);
            resp->setStatusCode(k409Conflict);
            callback(resp);
            return;
        }

        drogon_model::blog_db::Likes newLike;
        newLike.setPostId(postId);
        newLike.setUserId(userIdOpt.value());

        mapper.insert(newLike);

        Json::Value ret;
        ret["message"] = "Post liked successfully";
        auto resp = HttpResponse::newHttpJsonResponse(ret);
        callback(resp);
    } catch (const DrogonDbException &e) {
        LOG_ERROR << "DB Error: " << e.base().what();
        Json::Value ret;
        ret["error"] = "Failed to like post";
        auto resp = HttpResponse::newHttpJsonResponse(ret);
        resp->setStatusCode(k500InternalServerError);
        callback(resp);
    }
}

void PostController::unlikePost(const HttpRequestPtr &req,
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

    auto dbClient = drogon::app().getDbClient();
    Mapper<drogon_model::blog_db::Likes> mapper(dbClient);

    try {
        auto likes = mapper.findBy(
            Criteria(drogon_model::blog_db::Likes::Cols::_post_id, 
                    CompareOperator::EQ, postId) &&
            Criteria(drogon_model::blog_db::Likes::Cols::_user_id, 
                    CompareOperator::EQ, userIdOpt.value())
        );

        if (likes.size() == 0) {
            Json::Value ret;
            ret["error"] = "Like not found";
            auto resp = HttpResponse::newHttpJsonResponse(ret);
            resp->setStatusCode(k404NotFound);
            callback(resp);
            return;
        }

        mapper.deleteByPrimaryKey(likes[0].getValueOfId());

        Json::Value ret;
        ret["message"] = "Post unliked successfully";
        auto resp = HttpResponse::newHttpJsonResponse(ret);
        callback(resp);
    } catch (const DrogonDbException &e) {
        LOG_ERROR << "DB Error: " << e.base().what();
        Json::Value ret;
        ret["error"] = "Failed to unlike post";
        auto resp = HttpResponse::newHttpJsonResponse(ret);
        resp->setStatusCode(k500InternalServerError);
        callback(resp);
    }
}

void PostController::getLikesCount(const HttpRequestPtr &req,
                                  std::function<void(const HttpResponsePtr &)> &&callback,
                                  int postId)
{
    auto dbClient = drogon::app().getDbClient();
    Mapper<drogon_model::blog_db::Likes> mapper(dbClient);

    try {
        auto likes = mapper.findBy(
            Criteria(drogon_model::blog_db::Likes::Cols::_post_id,
                    CompareOperator::EQ, postId)
        );

        // ETag = (post_id, count). likes is just a join row that gets
        // created/dropped by like/unlike — count is the entire payload,
        // so any change yields a new ETag without further state.
        const std::string etag = http_cache::makeWeakEtag({
            "likes-count", std::to_string(postId),
            std::to_string(likes.size()),
        });
        if (http_cache::ifNoneMatchHit(req, etag)) {
            callback(http_cache::makeNotModified(etag));
            return;
        }

        Json::Value ret;
        ret["post_id"] = postId;
        ret["likes_count"] = (int)likes.size();

        auto resp = HttpResponse::newHttpJsonResponse(ret);
        http_cache::applyCacheHeaders(resp, etag);
        callback(resp);
    } catch (const DrogonDbException &e) {
        LOG_ERROR << "DB Error: " << e.base().what();
        Json::Value ret;
        ret["error"] = "Failed to get likes count";
        auto resp = HttpResponse::newHttpJsonResponse(ret);
        resp->setStatusCode(k500InternalServerError);
        callback(resp);
    }
}
