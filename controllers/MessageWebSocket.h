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
    // in sync). The payload mirrors what /messages would return.
    //
    // Invoked by the PgListener bridge when an INSERT into `messages` fires
    // the trg_messages_notify trigger — never called by request handlers.
    static void pushNewMessage(int receiverId,
                               int senderId,
                               const Json::Value& msg);

    // Push a freshly-persisted comment to every WebSocket connection that
    // has subscribed to `postId` via {"type":"subscribe_post"} from the
    // client. Invoked by the PgListener bridge on trg_comments_notify.
    static void pushNewComment(int postId, const Json::Value& comment);

    // Nudge one user's open connections that a notification landed. Only the
    // kind travels — the client refetches the list. Pushing the whole row
    // would put a second, divergent copy of the notification in the client
    // and mean the WebSocket and the REST endpoint could disagree about
    // what is unread.
    static void pushNotification(int userId, const std::string& kind);

    // Number of currently-connected sockets (for /metrics). Cheap mutex-only.
    static std::size_t connectionCount();

    // Close every open WebSocket with a clean kNormalClosure frame. The
    // SPA's reconnect logic picks up the new pod after a short backoff
    // (see frontend_app/src/stores/messages.ts) so users barely notice
    // the cycle. Called from the SIGTERM handler before app().quit().
    // Safe to call multiple times — the registry empties on first run.
    static void shutdownAll();
};
