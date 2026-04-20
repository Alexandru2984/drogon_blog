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
    Mapper<drogon_model::sqlite3::Posts> postMapper(dbClient);
    Mapper<drogon_model::sqlite3::Users> userMapper(dbClient);

    try {
        auto posts = postMapper.orderBy(drogon_model::sqlite3::Posts::Cols::_created_at, 
                                       SortOrder::DESC).findAll();

        Json::Value ret;
        ret["posts"] = Json::Value(Json::arrayValue);

        for (const auto &post : posts) {
            Json::Value postJson;
            postJson["id"] = post.getValueOfId();
            postJson["title"] = post.getValueOfTitle();
            postJson["content"] = post.getValueOfContent();
            postJson["created_at"] = post.getValueOfCreatedAt().toDbStringLocal();
            postJson["updated_at"] = post.getValueOfUpdatedAt().toDbStringLocal();
            
            // Get author info
            try {
                auto author = userMapper.findByPrimaryKey(post.getValueOfUserId());
                postJson["author"]["id"] = author.getValueOfId();
                postJson["author"]["username"] = author.getValueOfUsername();
                if (!author.getValueOfProfileImage().empty()) {
                    postJson["author"]["profile_image"] = author.getValueOfProfileImage();
                }
            } catch (...) {}

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

void PostController::getPost(const HttpRequestPtr &req,
                            std::function<void(const HttpResponsePtr &)> &&callback,
                            int postId)
{
    auto dbClient = drogon::app().getDbClient();
    Mapper<drogon_model::sqlite3::Posts> postMapper(dbClient);
    Mapper<drogon_model::sqlite3::Users> userMapper(dbClient);

    try {
        auto post = postMapper.findByPrimaryKey(postId);

        Json::Value ret;
        ret["id"] = post.getValueOfId();
        ret["title"] = post.getValueOfTitle();
        ret["content"] = post.getValueOfContent();
        ret["created_at"] = post.getValueOfCreatedAt().toDbStringLocal();
        ret["updated_at"] = post.getValueOfUpdatedAt().toDbStringLocal();

        // Get author info
        try {
            auto author = userMapper.findByPrimaryKey(post.getValueOfUserId());
            ret["author"]["id"] = author.getValueOfId();
            ret["author"]["username"] = author.getValueOfUsername();
            if (!author.getValueOfProfileImage().empty()) {
                ret["author"]["profile_image"] = author.getValueOfProfileImage();
            }
        } catch (...) {}

        auto resp = HttpResponse::newHttpJsonResponse(ret);
        callback(resp);
    } catch (const DrogonDbException &e) {
        Json::Value ret;
        ret["error"] = "Post not found";
        auto resp = HttpResponse::newHttpJsonResponse(ret);
        resp->setStatusCode(k404NotFound);
        callback(resp);
    }
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
    Mapper<drogon_model::sqlite3::Posts> mapper(dbClient);

    drogon_model::sqlite3::Posts newPost;
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
    Mapper<drogon_model::sqlite3::Posts> mapper(dbClient);

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
    Mapper<drogon_model::sqlite3::Posts> mapper(dbClient);

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
    Mapper<drogon_model::sqlite3::Posts> mapper(dbClient);

    try {
        auto posts = mapper.findBy(
            Criteria(drogon_model::sqlite3::Posts::Cols::_user_id, 
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
    Mapper<drogon_model::sqlite3::Likes> mapper(dbClient);

    try {
        // Check if already liked
        auto existingLikes = mapper.findBy(
            Criteria(drogon_model::sqlite3::Likes::Cols::_post_id, 
                    CompareOperator::EQ, postId) &&
            Criteria(drogon_model::sqlite3::Likes::Cols::_user_id, 
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

        drogon_model::sqlite3::Likes newLike;
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
    Mapper<drogon_model::sqlite3::Likes> mapper(dbClient);

    try {
        auto likes = mapper.findBy(
            Criteria(drogon_model::sqlite3::Likes::Cols::_post_id, 
                    CompareOperator::EQ, postId) &&
            Criteria(drogon_model::sqlite3::Likes::Cols::_user_id, 
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
    Mapper<drogon_model::sqlite3::Likes> mapper(dbClient);

    try {
        auto likes = mapper.findBy(
            Criteria(drogon_model::sqlite3::Likes::Cols::_post_id, 
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
