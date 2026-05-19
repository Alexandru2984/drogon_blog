#include "PostController.h"
#include "../models/Posts.h"
#include "../models/Users.h"
#include "../models/Likes.h"
#include <drogon/orm/Mapper.h>
#include <trantor/utils/Logger.h>

using namespace drogon;
using namespace drogon::orm;

void PostController::getAllPosts(const HttpRequestPtr &req,
                                std::function<void(const HttpResponsePtr &)> &&callback)
{
    auto dbClient = drogon::app().getDbClient();

    // Single JOIN — author resolved in one round-trip instead of N+1.
    static const char* kSql =
        "SELECT p.id, p.title, p.content, p.created_at, p.updated_at, "
        "       u.id AS author_id, u.username AS author_username, u.profile_image AS author_profile_image "
        "FROM posts p "
        "LEFT JOIN users u ON u.id = p.user_id "
        "ORDER BY p.created_at DESC";

    dbClient->execSqlAsync(
        kSql,
        [callback](const Result& r) {
            Json::Value ret;
            ret["posts"] = Json::Value(Json::arrayValue);

            for (const auto& row : r) {
                Json::Value post;
                post["id"]         = row["id"].as<int64_t>();
                post["title"]      = row["title"].as<std::string>();
                post["content"]    = row["content"].as<std::string>();
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

            callback(HttpResponse::newHttpJsonResponse(ret));
        },
        [callback](const DrogonDbException& e) {
            LOG_ERROR << "DB Error (getAllPosts): " << e.base().what();
            Json::Value ret;
            ret["error"] = "Failed to fetch posts";
            auto resp = HttpResponse::newHttpJsonResponse(ret);
            resp->setStatusCode(k500InternalServerError);
            callback(resp);
        });
}

void PostController::getPost(const HttpRequestPtr &req,
                            std::function<void(const HttpResponsePtr &)> &&callback,
                            int postId)
{
    auto dbClient = drogon::app().getDbClient();

    static const char* kSql =
        "SELECT p.id, p.title, p.content, p.created_at, p.updated_at, "
        "       u.id AS author_id, u.username AS author_username, u.profile_image AS author_profile_image "
        "FROM posts p "
        "LEFT JOIN users u ON u.id = p.user_id "
        "WHERE p.id = $1";

    dbClient->execSqlAsync(
        kSql,
        [callback](const Result& r) {
            if (r.empty()) {
                Json::Value ret;
                ret["error"] = "Post not found";
                auto resp = HttpResponse::newHttpJsonResponse(ret);
                resp->setStatusCode(k404NotFound);
                callback(resp);
                return;
            }
            const auto& row = r[0];
            Json::Value ret;
            ret["id"]         = row["id"].as<int64_t>();
            ret["title"]      = row["title"].as<std::string>();
            ret["content"]    = row["content"].as<std::string>();
            ret["created_at"] = row["created_at"].as<std::string>();
            ret["updated_at"] = row["updated_at"].as<std::string>();

            if (!row["author_id"].isNull()) {
                ret["author"]["id"]       = row["author_id"].as<int64_t>();
                ret["author"]["username"] = row["author_username"].as<std::string>();
                if (!row["author_profile_image"].isNull()) {
                    auto img = row["author_profile_image"].as<std::string>();
                    if (!img.empty()) ret["author"]["profile_image"] = img;
                }
            }
            callback(HttpResponse::newHttpJsonResponse(ret));
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

    drogon_model::blog_db::Posts newPost;
    newPost.setUserId(userIdOpt.value());
    newPost.setTitle(title);
    newPost.setContent(content);

    try {
        mapper.insert(newPost);

        Json::Value ret;
        ret["message"] = "Post created successfully";
        ret["post"]["id"] = newPost.getValueOfId();
        ret["post"]["title"] = newPost.getValueOfTitle();
        ret["post"]["content"] = newPost.getValueOfContent();

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
            post.setContent((*json)["content"].asString());
        }

        mapper.update(post);

        Json::Value ret;
        ret["message"] = "Post updated successfully";
        ret["post"]["id"] = post.getValueOfId();
        ret["post"]["title"] = post.getValueOfTitle();
        ret["post"]["content"] = post.getValueOfContent();

        auto resp = HttpResponse::newHttpJsonResponse(ret);
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

        Json::Value ret;
        ret["message"] = "Post deleted successfully";
        auto resp = HttpResponse::newHttpJsonResponse(ret);
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

        Json::Value ret;
        ret["post_id"] = postId;
        ret["likes_count"] = (int)likes.size();

        auto resp = HttpResponse::newHttpJsonResponse(ret);
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
