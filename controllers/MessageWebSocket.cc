#include "MessageWebSocket.h"
#include "../helpers/Presence.h"

#include <drogon/drogon.h>
#include <trantor/utils/Logger.h>

#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

using namespace drogon;

namespace {

// CSWSH defence: browsers send cookies on cross-origin WebSocket opens
// the same way they would on a top-level navigation, so the only thing
// stopping evil.com from hijacking a logged-in session is an explicit
// Origin check at handshake time. We reject anything that isn't an
// exact byte-for-byte match for BLOG_SITE_ORIGIN. A missing Origin
// header (curl, native apps, server-to-server tests) is allowed
// because those agents don't carry session cookies anyway and the
// auth check below is the actual gate for them.
bool originAllowed(const HttpRequestPtr& req)
{
    const std::string& origin = req->getHeader("Origin");
    if (origin.empty()) return true;
    const char* expected = std::getenv("BLOG_SITE_ORIGIN");
    if (!expected || !*expected) {
        // Fail closed: if the operator forgot to set the origin, refuse
        // every cross-origin handshake rather than allowing all.
        return false;
    }
    return origin == std::string(expected);
}

} // namespace

namespace {

// Per-process hub: which user IDs currently have which WebSocket connections,
// and which posts each connection has subscribed to for live comments.
// Connections are keyed by their shared_ptr identity so the set entries keep
// the connection alive while it's open.
std::mutex                                                                       g_mu;
std::unordered_map<int, std::unordered_set<WebSocketConnectionPtr>>              g_byUser;
std::unordered_map<int, std::unordered_set<WebSocketConnectionPtr>>              g_byPost;

struct ConnCtx {
    // cppcheck-suppress unusedStructMember  // read via ctx->userId after shared_ptr deref
    int                          userId;
    std::unordered_set<int>      subscribedPosts;   // protected by g_mu
};

void registerConnection(int userId, const WebSocketConnectionPtr& conn)
{
    std::lock_guard<std::mutex> lk(g_mu);
    g_byUser[userId].insert(conn);
}

void unregisterConnection(const WebSocketConnectionPtr& conn)
{
    auto ctx = conn->getContext<ConnCtx>();
    if (!ctx) return;
    std::lock_guard<std::mutex> lk(g_mu);

    auto userIt = g_byUser.find(ctx->userId);
    if (userIt != g_byUser.end()) {
        userIt->second.erase(conn);
        if (userIt->second.empty()) g_byUser.erase(userIt);
    }
    for (int postId : ctx->subscribedPosts) {
        auto postIt = g_byPost.find(postId);
        if (postIt == g_byPost.end()) continue;
        postIt->second.erase(conn);
        if (postIt->second.empty()) g_byPost.erase(postIt);
    }
    ctx->subscribedPosts.clear();
}

// Hard cap on per-connection post subscriptions. Without this, a client
// can loop {"type":"subscribe_post","post_id":N} for N in 1..N and grow
// both the per-connection set and the global g_byPost map without
// bound — straightforward OOM kill of the Drogon process. 50 is well
// above any plausible UI use (the human can read maybe half a dozen
// posts live at once).
constexpr std::size_t kMaxSubscribedPostsPerConn = 50;

void subscribeToPost(const WebSocketConnectionPtr& conn, int postId)
{
    auto ctx = conn->getContext<ConnCtx>();
    if (!ctx) return;
    std::lock_guard<std::mutex> lk(g_mu);
    if (ctx->subscribedPosts.size() >= kMaxSubscribedPostsPerConn) return;
    if (ctx->subscribedPosts.insert(postId).second) {
        g_byPost[postId].insert(conn);
    }
}

void unsubscribeFromPost(const WebSocketConnectionPtr& conn, int postId)
{
    auto ctx = conn->getContext<ConnCtx>();
    if (!ctx) return;
    std::lock_guard<std::mutex> lk(g_mu);
    if (ctx->subscribedPosts.erase(postId)) {
        auto it = g_byPost.find(postId);
        if (it != g_byPost.end()) {
            it->second.erase(conn);
            if (it->second.empty()) g_byPost.erase(it);
        }
    }
}

// Snapshot the live conns for a key so we can send outside the lock.
std::vector<WebSocketConnectionPtr> connectionsForUser(int userId)
{
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_byUser.find(userId);
    if (it == g_byUser.end()) return {};
    return {it->second.begin(), it->second.end()};
}

std::vector<WebSocketConnectionPtr> connectionsForPost(int postId)
{
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_byPost.find(postId);
    if (it == g_byPost.end()) return {};
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
    // CSWSH guard runs BEFORE the auth check so a hostile origin can't
    // even learn whether a session is valid via the close code.
    if (!originAllowed(req)) {
        LOG_WARN << "rejecting WS handshake from disallowed origin '"
                 << req->getHeader("Origin") << "' peer="
                 << req->getPeerAddr().toIpPort();
        conn->shutdown(CloseCode::kViolation, "origin not allowed");
        return;
    }

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

    conn->setContext(std::make_shared<ConnCtx>(ConnCtx{userIdOpt.value(), {}}));
    registerConnection(userIdOpt.value(), conn);
    presence::markOnline(userIdOpt.value());

    Json::Value hello;
    hello["type"]    = "ready";
    hello["user_id"] = userIdOpt.value();
    conn->send(toJsonLine(hello), WebSocketMessageType::Text);
}

void MessageWebSocket::handleNewMessage(const WebSocketConnectionPtr& conn,
                                        std::string&& msg,
                                        const WebSocketMessageType& type)
{
    if (type == WebSocketMessageType::Pong) return;
    if (msg == "ping") {
        conn->send("pong", WebSocketMessageType::Text);
        return;
    }
    // Lightweight protocol: client may send small JSON control messages to
    // (un)subscribe from per-post comment feeds. Everything else is ignored
    // — writes still go through REST so server-side checks live in one place.
    Json::CharReaderBuilder rb;
    Json::Value             root;
    std::string             errs;
    std::istringstream      iss(msg);
    if (!Json::parseFromStream(rb, iss, &root, &errs)) return;

    const std::string typ = root.get("type", "").asString();
    if (typ == "subscribe_post" && root.isMember("post_id") &&
        root["post_id"].isInt())
    {
        subscribeToPost(conn, root["post_id"].asInt());
    }
    else if (typ == "unsubscribe_post" && root.isMember("post_id") &&
             root["post_id"].isInt())
    {
        unsubscribeFromPost(conn, root["post_id"].asInt());
    }
}

// Only the LAST connection's close should flip the user offline —
// a user may have multiple tabs open. We snapshot userId before
// unregistering, then check whether they still have any sockets
// remaining on this pod.
namespace {
bool stillLocallyOnline(int userId)
{
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_byUser.find(userId);
    return it != g_byUser.end() && !it->second.empty();
}
} // namespace

void MessageWebSocket::handleConnectionClosed(const WebSocketConnectionPtr& conn)
{
    auto ctx = conn->getContext<ConnCtx>();
    const int userId = ctx ? ctx->userId : 0;
    unregisterConnection(conn);
    // Only flip the user offline when this pod has no other live
    // socket for them. A user with multiple tabs / devices stays
    // online until the last one disconnects.
    if (userId > 0 && !stillLocallyOnline(userId)) {
        presence::markOffline(userId);
    }
}

void MessageWebSocket::pushNewMessage(int receiverId,
                                      int senderId,
                                      const Json::Value& msg)
{
    Json::Value envelope;
    envelope["type"]    = "message";
    envelope["message"] = msg;
    const std::string payload = toJsonLine(envelope);

    sendOnAll(connectionsForUser(receiverId), payload);
    if (senderId != receiverId) {
        sendOnAll(connectionsForUser(senderId), payload);
    }
}

void MessageWebSocket::pushNewComment(int postId, const Json::Value& comment)
{
    Json::Value envelope;
    envelope["type"]    = "comment";
    envelope["post_id"] = postId;
    envelope["comment"] = comment;
    sendOnAll(connectionsForPost(postId), toJsonLine(envelope));
}

std::size_t MessageWebSocket::connectionCount()
{
    std::lock_guard<std::mutex> lk(g_mu);
    std::size_t total = 0;
    for (const auto& entry : g_byUser) total += entry.second.size();
    return total;
}

void MessageWebSocket::shutdownAll()
{
    // Snapshot the connections under the lock, then close them outside
    // it. shutdown() ultimately calls back into our handleConnectionClosed
    // which tries to reacquire g_mu via unregisterConnection — holding
    // the lock across shutdown() would deadlock.
    std::vector<WebSocketConnectionPtr> conns;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        conns.reserve(g_byUser.size());
        // Iterate by value; we only need the connection set, the user-id
        // key is unused here. (Older cppcheck flags `auto& [_, set]` as
        // an unused-variable false-positive on the discarded binding.)
        for (auto& entry : g_byUser) {
            for (const auto& c : entry.second) conns.push_back(c);
        }
        g_byUser.clear();
        g_byPost.clear();
    }
    for (const auto& c : conns) {
        c->shutdown(CloseCode::kNormalClosure, "server shutting down");
    }
}
