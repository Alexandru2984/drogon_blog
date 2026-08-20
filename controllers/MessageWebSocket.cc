#include "MessageWebSocket.h"
#include "../helpers/Presence.h"
#include "../helpers/Sessions.h"

#include <drogon/drogon.h>
#include <trantor/utils/Logger.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
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
// Third index, keyed by the session that opened the socket, so a revoked
// session can be hung up without also cutting the user's other devices.
// One sid can hold several sockets (a second tab reuses the session cookie).
std::unordered_map<std::string, std::unordered_set<WebSocketConnectionPtr>>      g_bySid;

// One authenticated browser session normally owns one socket; several tabs
// are legitimate, thousands are not. Without these limits a single account
// could consume Drogon's process-wide 20k connection budget and every file
// descriptor behind it.
constexpr std::size_t kMaxConnectionsPerSession = 8;
constexpr std::size_t kMaxConnectionsPerUser    = 16;

// The protocol only accepts tiny subscription controls and the literal
// "ping". Keep both per-frame work and sustained JSON parsing bounded.
constexpr std::size_t kMaxControlMessageBytes = 2048;
constexpr double      kControlMessageBurst    = 30.0;
constexpr double      kControlMessagesPerSec  = 10.0;

std::atomic<std::uint64_t> g_rejectedConnections{0};
std::atomic<std::uint64_t> g_policyClosures{0};

struct ConnCtx {
    // cppcheck-suppress unusedStructMember  // read via ctx->userId after shared_ptr deref
    int                          userId;
    // The sid recorded at handshake time. Empty only for a session that
    // predates sessions::begin() — those cannot be revoked either way.
    // cppcheck-suppress unusedStructMember  // read via ctx->sid after shared_ptr deref
    std::string                  sid;
    std::unordered_set<int>      subscribedPosts;   // protected by g_mu
    double                       controlTokens{kControlMessageBurst};
    std::chrono::steady_clock::time_point controlLastRefill{
        std::chrono::steady_clock::now()};
};

enum class RegistrationResult {
    Accepted,
    SessionLimit,
    UserLimit,
};

RegistrationResult registerConnection(int userId, const std::string& sid,
                                      const WebSocketConnectionPtr& conn)
{
    std::lock_guard<std::mutex> lk(g_mu);

    const auto userIt = g_byUser.find(userId);
    if (userIt != g_byUser.end() &&
        userIt->second.size() >= kMaxConnectionsPerUser)
    {
        return RegistrationResult::UserLimit;
    }

    if (!sid.empty()) {
        const auto sidIt = g_bySid.find(sid);
        if (sidIt != g_bySid.end() &&
            sidIt->second.size() >= kMaxConnectionsPerSession)
        {
            return RegistrationResult::SessionLimit;
        }
    }

    g_byUser[userId].insert(conn);
    if (!sid.empty()) g_bySid[sid].insert(conn);
    return RegistrationResult::Accepted;
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
    if (!ctx->sid.empty()) {
        auto sidIt = g_bySid.find(ctx->sid);
        if (sidIt != g_bySid.end()) {
            sidIt->second.erase(conn);
            if (sidIt->second.empty()) g_bySid.erase(sidIt);
        }
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

bool takeControlMessageToken(const WebSocketConnectionPtr& conn)
{
    auto ctx = conn->getContext<ConnCtx>();
    if (!ctx) return false;

    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lk(g_mu);
    const double elapsed = std::chrono::duration<double>(
        now - ctx->controlLastRefill).count();
    ctx->controlTokens = std::min(
        kControlMessageBurst,
        ctx->controlTokens + elapsed * kControlMessagesPerSec);
    ctx->controlLastRefill = now;
    if (ctx->controlTokens < 1.0) return false;
    ctx->controlTokens -= 1.0;
    return true;
}

void recordPolicyClosure(const WebSocketConnectionPtr& conn,
                         const char* reason)
{
    const auto total = g_policyClosures.fetch_add(
        1, std::memory_order_relaxed) + 1;
    // Logarithmic sampling prevents the defence itself becoming a log-volume
    // DoS while still leaving an immediate and progressively visible signal.
    if ((total & (total - 1)) == 0) {
        LOG_WARN << "websocket policy closures reached " << total
                 << "; latest reason=" << reason;
    }
    conn->shutdown(CloseCode::kViolation, reason);
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

    // Record which session this socket belongs to, so revoking that session
    // can find it later. Without the sid the connection is unrevocable: the
    // handshake is the only authentication it ever performs.
    const std::string sid = sessions::currentSid(req).value_or(std::string{});

    conn->setContext(std::make_shared<ConnCtx>(
        ConnCtx{userIdOpt.value(), sid, {}}));
    const auto registered = registerConnection(
        userIdOpt.value(), sid, conn);
    if (registered != RegistrationResult::Accepted) {
        const auto total = g_rejectedConnections.fetch_add(
            1, std::memory_order_relaxed) + 1;
        if ((total & (total - 1)) == 0) {
            LOG_WARN << "websocket connection-limit rejections reached "
                     << total;
        }
        conn->shutdown(
            CloseCode::kViolation,
            registered == RegistrationResult::SessionLimit
                ? "session connection limit"
                : "account connection limit");
        return;
    }
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
    // Ping is a valid WebSocket control frame. Drogon handles the protocol
    // response; we only budget how often the application is interrupted so
    // a standards-compliant heartbeat is accepted without becoming a CPU
    // bypass around the per-connection message rate.
    if (type == WebSocketMessageType::Ping) {
        if (!takeControlMessageToken(conn)) {
            recordPolicyClosure(conn, "control message rate exceeded");
        }
        return;
    }
    if (type != WebSocketMessageType::Text) {
        recordPolicyClosure(conn, "text frames only");
        return;
    }
    if (msg.size() > kMaxControlMessageBytes) {
        recordPolicyClosure(conn, "control message too large");
        return;
    }
    if (!takeControlMessageToken(conn)) {
        recordPolicyClosure(conn, "control message rate exceeded");
        return;
    }
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
        const int postId = root["post_id"].asInt();
        if (postId > 0) subscribeToPost(conn, postId);
    }
    else if (typ == "unsubscribe_post" && root.isMember("post_id") &&
             root["post_id"].isInt())
    {
        const int postId = root["post_id"].asInt();
        if (postId > 0) unsubscribeFromPost(conn, postId);
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

void MessageWebSocket::pushNotification(int userId, const std::string& kind)
{
    Json::Value envelope;
    envelope["type"] = "notification";
    envelope["kind"] = kind;
    sendOnAll(connectionsForUser(userId), toJsonLine(envelope));
}

void MessageWebSocket::closeForSession(const std::string& sid)
{
    if (sid.empty()) return;

    // Snapshot under the lock and shut down outside it, for the reason
    // spelled out in shutdownAll(): shutdown() calls back into
    // handleConnectionClosed on this thread, which reacquires g_mu.
    std::vector<WebSocketConnectionPtr> conns;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        auto it = g_bySid.find(sid);
        if (it == g_bySid.end()) return;
        conns.assign(it->second.begin(), it->second.end());
    }

    // 1008 rather than a normal closure: the client's reconnect logic backs
    // off and retries after a clean close, which for a revoked session would
    // be a reconnect loop against a cookie that no longer authenticates.
    for (const auto& c : conns) {
        if (c) c->shutdown(CloseCode::kViolation, "session revoked");
    }
    LOG_INFO << "closed " << conns.size()
             << " websocket(s) for revoked session";
}

std::size_t MessageWebSocket::connectionCount()
{
    std::lock_guard<std::mutex> lk(g_mu);
    std::size_t total = 0;
    for (const auto& entry : g_byUser) total += entry.second.size();
    return total;
}

std::uint64_t MessageWebSocket::rejectedConnectionCount()
{
    return g_rejectedConnections.load(std::memory_order_relaxed);
}

std::uint64_t MessageWebSocket::policyClosureCount()
{
    return g_policyClosures.load(std::memory_order_relaxed);
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
        for (const auto& entry : g_byUser) {
            for (const auto& c : entry.second) conns.push_back(c);
        }
        g_byUser.clear();
        g_byPost.clear();
        g_bySid.clear();
    }
    for (const auto& c : conns) {
        c->shutdown(CloseCode::kNormalClosure, "server shutting down");
    }
}
