#include "CommentController.h"
#include "../models/Comments.h"
#include "../models/Users.h"
#include <drogon/orm/Mapper.h>
#include <trantor/utils/Logger.h>

using namespace drogon;
using namespace drogon::orm;

void CommentController::getPostComments(const HttpRequestPtr &req,
                                       std::function<void(const HttpResponsePtr &)> &&callback,
                                       int postId)
{
    auto dbClient = drogon::app().getDbClient();
    Mapper<drogon_model::sqlite3::Comments> commentMapper(dbClient);
    Mapper<drogon_model::sqlite3::Users> userMapper(dbClient);

    try {
        auto comments = commentMapper.orderBy(drogon_model::sqlite3::Comments::Cols::_created_at, 
                                             SortOrder::ASC)
                                    .findBy(Criteria(drogon_model::sqlite3::Comments::Cols::_post_id, 
                                                    CompareOperator::EQ, postId));

        Json::Value ret;
        ret["comments"] = Json::Value(Json::arrayValue);

        for (const auto &comment : comments) {
            Json::Value commentJson;
            commentJson["id"] = comment.getValueOfId();
            commentJson["content"] = comment.getValueOfContent();
            commentJson["created_at"] = comment.getValueOfCreatedAt().toDbStringLocal();
            
            // Get author info
            try {
                auto author = userMapper.findByPrimaryKey(comment.getValueOfUserId());
                commentJson["author"]["id"] = author.getValueOfId();
                commentJson["author"]["username"] = author.getValueOfUsername();
                if (!author.getValueOfProfileImage().empty()) {
                    commentJson["author"]["profile_image"] = author.getValueOfProfileImage();
                }
            } catch (...) {}

            ret["comments"].append(commentJson);
        }

        auto resp = HttpResponse::newHttpJsonResponse(ret);
        callback(resp);
    } catch (const DrogonDbException &e) {
        LOG_ERROR << "DB Error: " << e.base().what();
        Json::Value ret;
        ret["error"] = "Failed to fetch comments";
        auto resp = HttpResponse::newHttpJsonResponse(ret);
        resp->setStatusCode(k500InternalServerError);
        callback(resp);
    }
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

    auto dbClient = drogon::app().getDbClient();
    Mapper<drogon_model::sqlite3::Comments> mapper(dbClient);

    drogon_model::sqlite3::Comments newComment;
    newComment.setPostId(postId);
    newComment.setUserId(userIdOpt.value());
    newComment.setContent(content);

    try {
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
    Mapper<drogon_model::sqlite3::Comments> mapper(dbClient);

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
            comment.setContent((*json)["content"].asString());
        }

        mapper.update(comment);

        Json::Value ret;
        ret["message"] = "Comment updated successfully";
        ret["comment"]["id"] = comment.getValueOfId();
        ret["comment"]["content"] = comment.getValueOfContent();

        auto resp = HttpResponse::newHttpJsonResponse(ret);
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
    Mapper<drogon_model::sqlite3::Comments> mapper(dbClient);

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
    } catch (const DrogonDbException &e) {
        LOG_ERROR << "DB Error: " << e.base().what();
        Json::Value ret;
        ret["error"] = "Failed to delete comment";
        auto resp = HttpResponse::newHttpJsonResponse(ret);
        resp->setStatusCode(k500InternalServerError);
        callback(resp);
    }
}
