#include "MessageController.h"
#include "../models/Messages.h"
#include "../models/Users.h"
#include <drogon/orm/Mapper.h>
#include <trantor/utils/Logger.h>

using namespace drogon;
using namespace drogon::orm;

void MessageController::getReceivedMessages(const HttpRequestPtr &req,
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

    auto dbClient = drogon::app().getDbClient();
    Mapper<drogon_model::blog_db::Messages> messageMapper(dbClient);
    Mapper<drogon_model::blog_db::Users> userMapper(dbClient);

    try {
        auto messages = messageMapper.orderBy(drogon_model::blog_db::Messages::Cols::_created_at, 
                                             SortOrder::DESC)
                                    .findBy(Criteria(drogon_model::blog_db::Messages::Cols::_receiver_id, 
                                                    CompareOperator::EQ, userIdOpt.value()));

        Json::Value ret;
        ret["messages"] = Json::Value(Json::arrayValue);

        for (const auto &message : messages) {
            Json::Value msgJson;
            msgJson["id"] = message.getValueOfId();
            msgJson["content"] = message.getValueOfContent();
            msgJson["is_read"] = message.getValueOfIsRead();
            msgJson["created_at"] = message.getValueOfCreatedAt().toDbStringLocal();
            
            // Get sender info
            try {
                auto sender = userMapper.findByPrimaryKey(message.getValueOfSenderId());
                msgJson["sender"]["id"] = sender.getValueOfId();
                msgJson["sender"]["username"] = sender.getValueOfUsername();
                if (!sender.getValueOfProfileImage().empty()) {
                    msgJson["sender"]["profile_image"] = sender.getValueOfProfileImage();
                }
            } catch (...) {}

            ret["messages"].append(msgJson);
        }

        auto resp = HttpResponse::newHttpJsonResponse(ret);
        callback(resp);
    } catch (const DrogonDbException &e) {
        LOG_ERROR << "DB Error: " << e.base().what();
        Json::Value ret;
        ret["error"] = "Failed to fetch messages";
        auto resp = HttpResponse::newHttpJsonResponse(ret);
        resp->setStatusCode(k500InternalServerError);
        callback(resp);
    }
}

void MessageController::getSentMessages(const HttpRequestPtr &req,
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

    auto dbClient = drogon::app().getDbClient();
    Mapper<drogon_model::blog_db::Messages> messageMapper(dbClient);
    Mapper<drogon_model::blog_db::Users> userMapper(dbClient);

    try {
        auto messages = messageMapper.orderBy(drogon_model::blog_db::Messages::Cols::_created_at, 
                                             SortOrder::DESC)
                                    .findBy(Criteria(drogon_model::blog_db::Messages::Cols::_sender_id, 
                                                    CompareOperator::EQ, userIdOpt.value()));

        Json::Value ret;
        ret["messages"] = Json::Value(Json::arrayValue);

        for (const auto &message : messages) {
            Json::Value msgJson;
            msgJson["id"] = message.getValueOfId();
            msgJson["content"] = message.getValueOfContent();
            msgJson["is_read"] = message.getValueOfIsRead();
            msgJson["created_at"] = message.getValueOfCreatedAt().toDbStringLocal();
            
            // Get receiver info
            try {
                auto receiver = userMapper.findByPrimaryKey(message.getValueOfReceiverId());
                msgJson["receiver"]["id"] = receiver.getValueOfId();
                msgJson["receiver"]["username"] = receiver.getValueOfUsername();
                if (!receiver.getValueOfProfileImage().empty()) {
                    msgJson["receiver"]["profile_image"] = receiver.getValueOfProfileImage();
                }
            } catch (...) {}

            ret["messages"].append(msgJson);
        }

        auto resp = HttpResponse::newHttpJsonResponse(ret);
        callback(resp);
    } catch (const DrogonDbException &e) {
        LOG_ERROR << "DB Error: " << e.base().what();
        Json::Value ret;
        ret["error"] = "Failed to fetch messages";
        auto resp = HttpResponse::newHttpJsonResponse(ret);
        resp->setStatusCode(k500InternalServerError);
        callback(resp);
    }
}

void MessageController::getConversation(const HttpRequestPtr &req,
                                       std::function<void(const HttpResponsePtr &)> &&callback,
                                       int otherUserId)
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
    Mapper<drogon_model::blog_db::Messages> messageMapper(dbClient);
    Mapper<drogon_model::blog_db::Users> userMapper(dbClient);

    try {
        // Get messages between current user and other user
        auto messages = messageMapper.orderBy(drogon_model::blog_db::Messages::Cols::_created_at, 
                                             SortOrder::ASC)
                                    .findBy(
                                        (Criteria(drogon_model::blog_db::Messages::Cols::_sender_id, 
                                                CompareOperator::EQ, userIdOpt.value()) &&
                                         Criteria(drogon_model::blog_db::Messages::Cols::_receiver_id, 
                                                CompareOperator::EQ, otherUserId)) ||
                                        (Criteria(drogon_model::blog_db::Messages::Cols::_sender_id, 
                                                CompareOperator::EQ, otherUserId) &&
                                         Criteria(drogon_model::blog_db::Messages::Cols::_receiver_id, 
                                                CompareOperator::EQ, userIdOpt.value()))
                                    );

        Json::Value ret;
        ret["messages"] = Json::Value(Json::arrayValue);

        for (const auto &message : messages) {
            Json::Value msgJson;
            msgJson["id"] = message.getValueOfId();
            msgJson["content"] = message.getValueOfContent();
            msgJson["is_read"] = message.getValueOfIsRead();
            msgJson["created_at"] = message.getValueOfCreatedAt().toDbStringLocal();
            msgJson["sender_id"] = message.getValueOfSenderId();
            msgJson["receiver_id"] = message.getValueOfReceiverId();

            ret["messages"].append(msgJson);
        }

        // Get other user info
        try {
            auto otherUser = userMapper.findByPrimaryKey(otherUserId);
            ret["other_user"]["id"] = otherUser.getValueOfId();
            ret["other_user"]["username"] = otherUser.getValueOfUsername();
            if (!otherUser.getValueOfProfileImage().empty()) {
                ret["other_user"]["profile_image"] = otherUser.getValueOfProfileImage();
            }
        } catch (...) {}

        auto resp = HttpResponse::newHttpJsonResponse(ret);
        callback(resp);
    } catch (const DrogonDbException &e) {
        LOG_ERROR << "DB Error: " << e.base().what();
        Json::Value ret;
        ret["error"] = "Failed to fetch conversation";
        auto resp = HttpResponse::newHttpJsonResponse(ret);
        resp->setStatusCode(k500InternalServerError);
        callback(resp);
    }
}

void MessageController::sendMessage(const HttpRequestPtr &req,
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

    int receiverId = (*json)["receiver_id"].asInt();
    std::string content = (*json)["content"].asString();

    if (receiverId <= 0 || content.empty()) {
        Json::Value ret;
        ret["error"] = "Receiver ID and content are required";
        auto resp = HttpResponse::newHttpJsonResponse(ret);
        resp->setStatusCode(k400BadRequest);
        callback(resp);
        return;
    }

    auto dbClient = drogon::app().getDbClient();
    Mapper<drogon_model::blog_db::Messages> mapper(dbClient);

    drogon_model::blog_db::Messages newMessage;
    newMessage.setSenderId(userIdOpt.value());
    newMessage.setReceiverId(receiverId);
    newMessage.setContent(content);
    newMessage.setIsRead(0);

    try {
        mapper.insert(newMessage);

        Json::Value ret;
        ret["message"] = "Message sent successfully";
        ret["msg"]["id"] = newMessage.getValueOfId();
        ret["msg"]["content"] = newMessage.getValueOfContent();
        ret["msg"]["created_at"] = newMessage.getValueOfCreatedAt().toDbStringLocal();

        auto resp = HttpResponse::newHttpJsonResponse(ret);
        resp->setStatusCode(k201Created);
        callback(resp);
    } catch (const DrogonDbException &e) {
        LOG_ERROR << "DB Error: " << e.base().what();
        Json::Value ret;
        ret["error"] = "Failed to send message";
        auto resp = HttpResponse::newHttpJsonResponse(ret);
        resp->setStatusCode(k500InternalServerError);
        callback(resp);
    }
}

void MessageController::markAsRead(const HttpRequestPtr &req,
                                  std::function<void(const HttpResponsePtr &)> &&callback,
                                  int messageId)
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
    Mapper<drogon_model::blog_db::Messages> mapper(dbClient);

    try {
        auto message = mapper.findByPrimaryKey(messageId);

        // Check if user is the receiver
        if (message.getValueOfReceiverId() != userIdOpt.value()) {
            Json::Value ret;
            ret["error"] = "Unauthorized";
            auto resp = HttpResponse::newHttpJsonResponse(ret);
            resp->setStatusCode(k403Forbidden);
            callback(resp);
            return;
        }

        message.setIsRead(1);
        mapper.update(message);

        Json::Value ret;
        ret["message"] = "Message marked as read";
        auto resp = HttpResponse::newHttpJsonResponse(ret);
        callback(resp);
    } catch (const DrogonDbException &e) {
        LOG_ERROR << "DB Error: " << e.base().what();
        Json::Value ret;
        ret["error"] = "Failed to mark message as read";
        auto resp = HttpResponse::newHttpJsonResponse(ret);
        resp->setStatusCode(k500InternalServerError);
        callback(resp);
    }
}

void MessageController::deleteMessage(const HttpRequestPtr &req,
                                     std::function<void(const HttpResponsePtr &)> &&callback,
                                     int messageId)
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
    Mapper<drogon_model::blog_db::Messages> mapper(dbClient);

    try {
        auto message = mapper.findByPrimaryKey(messageId);

        // Check if user is sender or receiver
        if (message.getValueOfSenderId() != userIdOpt.value() &&
            message.getValueOfReceiverId() != userIdOpt.value()) {
            Json::Value ret;
            ret["error"] = "Unauthorized";
            auto resp = HttpResponse::newHttpJsonResponse(ret);
            resp->setStatusCode(k403Forbidden);
            callback(resp);
            return;
        }

        mapper.deleteByPrimaryKey(messageId);

        Json::Value ret;
        ret["message"] = "Message deleted successfully";
        auto resp = HttpResponse::newHttpJsonResponse(ret);
        callback(resp);
    } catch (const DrogonDbException &e) {
        LOG_ERROR << "DB Error: " << e.base().what();
        Json::Value ret;
        ret["error"] = "Failed to delete message";
        auto resp = HttpResponse::newHttpJsonResponse(ret);
        resp->setStatusCode(k500InternalServerError);
        callback(resp);
    }
}
