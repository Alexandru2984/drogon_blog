#pragma once

#include <drogon/WebSocketController.h>
#include <json/json.h>

#include <cstddef>
#include <cstdint>
#include <string>

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

    // Monotonic abuse counters surfaced through /metrics. Connection
    // rejections mean the per-session/user cap was hit; policy closures mean
    // a client exceeded the frame-size/rate protocol budget.
    static std::uint64_t rejectedConnectionCount();
    static std::uint64_t policyClosureCount();

    // Hang up every socket opened under `sid`, with close code 1008.
    //
    // A WebSocket authenticates once, at the handshake, and then lives
    // outside the request pipeline that enforces revocation — so a session
    // killed by logout, "sign out this device", a password change, a ban or
    // an account deletion stayed live on the socket, which is precisely the
    // channel carrying that user's private messages. Revoking a session has
    // to reach the connection it opened, not just the next HTTP request.
    //
    // Wired to sessions::setRevocationObserver in main(), so it fires for
    // local revocations and for ones another process announces on
    // blog_event alike. Idempotent; unknown sids are a no-op.
    static void closeForSession(const std::string& sid);

    // Close every open WebSocket with a clean kNormalClosure frame. The
    // SPA's reconnect logic picks up the new pod after a short backoff
    // (see frontend_app/src/stores/messages.ts) so users barely notice
    // the cycle. Called from the SIGTERM handler before app().quit().
    // Safe to call multiple times — the registry empties on first run.
    static void shutdownAll();
};
