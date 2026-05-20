#pragma once

#include <drogon/WebSocketController.h>
#include <json/json.h>

class MessageWebSocket
    : public drogon::WebSocketController<MessageWebSocket>
{
public:
    void handleNewConnection(const drogon::HttpRequestPtr&,
                             const drogon::WebSocketConnectionPtr&) override;
    void handleNewMessage(const drogon::WebSocketConnectionPtr&,
                          std::string&&,
                          const drogon::WebSocketMessageType&) override;
    void handleConnectionClosed(const drogon::WebSocketConnectionPtr&) override;

    WS_PATH_LIST_BEGIN
        WS_PATH_ADD("/ws/messages");
    WS_PATH_LIST_END

    // Fan-out a freshly-persisted message to any live WebSocket connections
    // belonging to the receiver (and the sender, so other tabs / devices stay
    // in sync). The payload mirrors what /messages would return. Called from
    // MessageController::sendMessage after the DB row is committed.
    static void pushNewMessage(int receiverId,
                               int senderId,
                               const Json::Value& msg);

    // Number of currently-connected sockets (for /metrics). Cheap mutex-only.
    static std::size_t connectionCount();
};
