#pragma once

#include <drogon/HttpController.h>

using namespace drogon;

class MessageController : public drogon::HttpController<MessageController>
{
  public:
    METHOD_LIST_BEGIN
    // Get received messages
    ADD_METHOD_TO(MessageController::getReceivedMessages, "/messages/received", Get);
    // Get sent messages
    ADD_METHOD_TO(MessageController::getSentMessages, "/messages/sent", Get);
    // Get conversation with a user
    ADD_METHOD_TO(MessageController::getConversation, "/messages/conversation/{1}", Get);
    // Send message
    ADD_METHOD_TO(MessageController::sendMessage, "/messages", Post);
    // Mark message as read
    ADD_METHOD_TO(MessageController::markAsRead, "/messages/{1}/read", Put);
    // Delete message
    ADD_METHOD_TO(MessageController::deleteMessage, "/messages/{1}", Delete);
    METHOD_LIST_END
    
    void getReceivedMessages(const HttpRequestPtr &req,
                            std::function<void(const HttpResponsePtr &)> &&callback);
    void getSentMessages(const HttpRequestPtr &req,
                        std::function<void(const HttpResponsePtr &)> &&callback);
    void getConversation(const HttpRequestPtr &req,
                        std::function<void(const HttpResponsePtr &)> &&callback,
                        int otherUserId);
    void sendMessage(const HttpRequestPtr &req,
                    std::function<void(const HttpResponsePtr &)> &&callback);
    void markAsRead(const HttpRequestPtr &req,
                   std::function<void(const HttpResponsePtr &)> &&callback,
                   int messageId);
    void deleteMessage(const HttpRequestPtr &req,
                      std::function<void(const HttpResponsePtr &)> &&callback,
                      int messageId);
};
