#include "MessageWebSocket.h"

#include <drogon/drogon.h>
#include <trantor/utils/Logger.h>

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

using namespace drogon;

namespace {

// Per-process hub: which user IDs currently have which WebSocket connections.
// Connections are keyed by the raw pointer of the WebSocketConnection — the
// shared_ptr is held in the set so it never goes away while connected.
std::mutex                                                                       g_mu;
std::unordered_map<int, std::unordered_set<WebSocketConnectionPtr>>              g_byUser;

struct ConnCtx {
    int userId;
};

void addConnection(int userId, const WebSocketConnectionPtr& conn)
{
    std::lock_guard<std::mutex> lk(g_mu);
    g_byUser[userId].insert(conn);
}

void removeConnection(const WebSocketConnectionPtr& conn)
{
    auto ctx = conn->getContext<ConnCtx>();
    if (!ctx) return;
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_byUser.find(ctx->userId);
    if (it == g_byUser.end()) return;
    it->second.erase(conn);
    if (it->second.empty()) g_byUser.erase(it);
}

// Snapshot the live conns for a user so we can send outside the lock.
std::vector<WebSocketConnectionPtr> connectionsFor(int userId)
{
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_byUser.find(userId);
    if (it == g_byUser.end()) return {};
    return {it->second.begin(), it->second.end()};
}

std::string toJsonLine(const Json::Value& v)
{
    Json::StreamWriterBuilder b;
    b["indentation"] = "";
    return Json::writeString(b, v);
}

void sendOnAll(const std::vector<WebSocketConnectionPtr>& conns,
               const std::string& payload)
{
    for (const auto& c : conns) {
        if (c && c->connected()) {
            c->send(payload, WebSocketMessageType::Text);
        }
    }
}

} // namespace

void MessageWebSocket::handleNewConnection(const HttpRequestPtr& req,
                                           const WebSocketConnectionPtr& conn)
{
    // Auth-gated handshake: the upgrading HTTP request must carry a valid
    // session cookie. We close 1008 (policy violation) for anonymous peers.
    auto session = req->session();
    auto userIdOpt = session ? session->getOptional<int>("user_id")
                             : std::optional<int>{};

    if (!userIdOpt.has_value()) {
        LOG_DEBUG << "rejecting anonymous WS handshake from "
                  << req->getPeerAddr().toIpPort();
        conn->shutdown(CloseCode::kViolation, "auth required");
        return;
    }

    conn->setContext(std::make_shared<ConnCtx>(ConnCtx{userIdOpt.value()}));
    addConnection(userIdOpt.value(), conn);

    Json::Value hello;
    hello["type"]    = "ready";
    hello["user_id"] = userIdOpt.value();
    conn->send(toJsonLine(hello), WebSocketMessageType::Text);
}

void MessageWebSocket::handleNewMessage(const WebSocketConnectionPtr& conn,
                                        std::string&& msg,
                                        const WebSocketMessageType& type)
{
    // We don't accept client-driven messaging over WS (writes still go
    // through the REST endpoint so server-side checks stay in one place).
    // The only thing we honour is a "ping" application-level keepalive.
    if (type == WebSocketMessageType::Pong) return;
    if (msg == "ping") {
        conn->send("pong", WebSocketMessageType::Text);
        return;
    }
    // Anything else is ignored on purpose.
}

void MessageWebSocket::handleConnectionClosed(const WebSocketConnectionPtr& conn)
{
    removeConnection(conn);
}

void MessageWebSocket::pushNewMessage(int receiverId,
                                      int senderId,
                                      const Json::Value& msg)
{
    Json::Value envelope;
    envelope["type"]    = "message";
    envelope["message"] = msg;
    const std::string payload = toJsonLine(envelope);

    // Receiver gets the live notification; the sender's other clients also
    // get a copy so their UI updates without a round-trip.
    sendOnAll(connectionsFor(receiverId), payload);
    if (senderId != receiverId) {
        sendOnAll(connectionsFor(senderId), payload);
    }
}

std::size_t MessageWebSocket::connectionCount()
{
    std::lock_guard<std::mutex> lk(g_mu);
    std::size_t total = 0;
    for (const auto& [_, conns] : g_byUser) total += conns.size();
    return total;
}
