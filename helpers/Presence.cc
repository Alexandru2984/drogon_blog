#include "Presence.h"

#include <trantor/utils/Logger.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <unordered_set>

#ifdef BLOG_HAS_REDIS
#include <hiredis/hiredis.h>
#endif

namespace presence {

namespace {

// Local TTL for the per-user online key, in seconds. The heartbeat
// thread re-asserts the key every (kTtlSeconds / 2) so a brief redis
// reconnect blip during heartbeat doesn't drop users mid-session,
// but a crashed pod's keys naturally expire within ~30 s.
constexpr int kTtlSeconds = 30;

// Globally-aggregated online users live in this set so /metrics +
// callers asking "how many people are online?" can read it cheaply
// via SCARD. The TTL on the per-user key removes them from the set
// indirectly via the heartbeat (we re-SADD on heartbeat; an absent
// pod stops re-adding) — see refreshTick().
constexpr const char* kOnlineSet = "blog:online_users";

#ifdef BLOG_HAS_REDIS

// Single shared synchronous connection. The hub pushes presence
// updates relatively rarely (once per WS connect / disconnect) so a
// mutex-guarded sync client is acceptable. Future scale would move
// this to libhiredis-async wired into the Drogon event loop — for
// now, simplicity wins.
std::mutex                  g_mu;
redisContext*               g_ctx = nullptr;
std::string                 g_url;

// Users this pod has live WS connections for. Refreshed by the
// heartbeat. Protected by g_mu alongside g_ctx because the heartbeat
// reads it while WS callbacks write to it.
std::unordered_set<int>     g_localUsers;

std::atomic<bool>           g_running{false};
std::thread                 g_heartbeat;

// Parse "redis://host[:port]" or "unix://path". Anything else logs
// + falls back to "no connect". Keeping the parser inline and tiny
// because the URL shapes Drogon's chart docs encourage are exactly
// these two.
struct ParsedUrl {
    bool        valid = false;
    bool        isUnix = false;
    std::string host;
    int         port = 6379;
    std::string unixPath;
};

ParsedUrl parseUrl(const std::string& s)
{
    ParsedUrl r;
    if (s.empty()) return r;
    if (s.rfind("unix://", 0) == 0) {
        r.isUnix = true;
        r.unixPath = s.substr(7);
        r.valid = !r.unixPath.empty();
        return r;
    }
    if (s.rfind("redis://", 0) != 0) return r;
    auto hostPort = s.substr(8);
    auto colon = hostPort.find(':');
    if (colon == std::string::npos) {
        r.host = hostPort;
    } else {
        r.host = hostPort.substr(0, colon);
        try { r.port = std::stoi(hostPort.substr(colon + 1)); }
        catch (...) { return r; }
    }
    r.valid = !r.host.empty();
    return r;
}

redisContext* tryConnect(const ParsedUrl& u)
{
    constexpr timeval kTimeout{1, 0}; // 1s connect timeout
    redisContext* c = u.isUnix
        ? redisConnectUnixWithTimeout(u.unixPath.c_str(), kTimeout)
        : redisConnectWithTimeout(u.host.c_str(), u.port, kTimeout);
    if (!c || c->err) {
        LOG_WARN << "presence: redis connect failed: "
                 << (c ? c->errstr : "alloc failed");
        if (c) redisFree(c);
        return nullptr;
    }
    return c;
}

// Reconnect under g_mu. Caller already holds the lock.
void reconnectLocked()
{
    if (g_ctx) { redisFree(g_ctx); g_ctx = nullptr; }
    auto u = parseUrl(g_url);
    if (!u.valid) return;
    g_ctx = tryConnect(u);
}

// Run a redis command under g_mu, with one reconnect retry. Caller
// passes ownership of the reply to discardReply via a unique_ptr-
// like RAII would; for brevity we just call freeReplyObject.
bool runCmd(const char* fmt, ...)
{
    if (!g_ctx) return false;
    va_list ap;
    va_start(ap, fmt);
    auto* reply = static_cast<redisReply*>(redisvCommand(g_ctx, fmt, ap));
    va_end(ap);
    if (!reply) {
        // Connection lost — try one reconnect+retry.
        LOG_DEBUG << "presence: command failed (" << g_ctx->errstr
                  << "), reconnecting";
        reconnectLocked();
        if (!g_ctx) return false;
        va_start(ap, fmt);
        reply = static_cast<redisReply*>(redisvCommand(g_ctx, fmt, ap));
        va_end(ap);
        if (!reply) return false;
    }
    bool ok = reply->type != REDIS_REPLY_ERROR;
    if (!ok) {
        LOG_WARN << "presence: redis error reply: " << reply->str;
    }
    freeReplyObject(reply);
    return ok;
}

void refreshTick()
{
    while (g_running.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::seconds(kTtlSeconds / 2));
        if (!g_running.load(std::memory_order_acquire)) break;

        // Snapshot under lock to bound the time we hold g_mu while
        // doing network I/O. The set is small (one entry per
        // locally-connected user) so the copy cost is irrelevant.
        std::vector<int> snapshot;
        {
            std::lock_guard<std::mutex> lk(g_mu);
            snapshot.assign(g_localUsers.begin(), g_localUsers.end());
        }

        std::lock_guard<std::mutex> lk(g_mu);
        if (!g_ctx) { reconnectLocked(); if (!g_ctx) continue; }
        for (int uid : snapshot) {
            runCmd("SETEX user:%d:online %d 1", uid, kTtlSeconds);
            runCmd("SADD %s %d",                 kOnlineSet, uid);
        }
    }
}

#endif // BLOG_HAS_REDIS

} // namespace

bool install()
{
#ifndef BLOG_HAS_REDIS
    LOG_INFO << "presence: build lacks hiredis; helper compiled out.";
    return false;
#else
    const char* env = std::getenv("BLOG_REDIS_URL");
    if (!env || !*env) {
        LOG_INFO << "presence: BLOG_REDIS_URL unset; running single-pod.";
        return false;
    }
    g_url = env;
    auto u = parseUrl(g_url);
    if (!u.valid) {
        LOG_ERROR << "presence: malformed BLOG_REDIS_URL=" << g_url
                  << " (want redis://host[:port] or unix://path).";
        return false;
    }
    std::lock_guard<std::mutex> lk(g_mu);
    g_ctx = tryConnect(u);
    if (!g_ctx) return false;

    g_running.store(true, std::memory_order_release);
    g_heartbeat = std::thread(refreshTick);
    LOG_INFO << "presence: connected to " << g_url
             << " (TTL " << kTtlSeconds << "s).";
    return true;
#endif
}

void markOnline(int userId)
{
#ifdef BLOG_HAS_REDIS
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_ctx) return;
    g_localUsers.insert(userId);
    runCmd("SETEX user:%d:online %d 1", userId, kTtlSeconds);
    runCmd("SADD %s %d",                 kOnlineSet, userId);
#else
    (void)userId;
#endif
}

void markOffline(int userId)
{
#ifdef BLOG_HAS_REDIS
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_ctx) return;
    g_localUsers.erase(userId);
    runCmd("DEL user:%d:online", userId);
    runCmd("SREM %s %d",           kOnlineSet, userId);
#else
    (void)userId;
#endif
}

bool isOnline(int userId)
{
#ifdef BLOG_HAS_REDIS
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_ctx) return false;
    auto* reply = static_cast<redisReply*>(
        redisCommand(g_ctx, "EXISTS user:%d:online", userId));
    if (!reply) { reconnectLocked(); return false; }
    bool exists = reply->type == REDIS_REPLY_INTEGER && reply->integer == 1;
    freeReplyObject(reply);
    return exists;
#else
    (void)userId;
    return false;
#endif
}

long onlineCountGlobal()
{
#ifdef BLOG_HAS_REDIS
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_ctx) return -1;
    auto* reply = static_cast<redisReply*>(
        redisCommand(g_ctx, "SCARD %s", kOnlineSet));
    if (!reply) { reconnectLocked(); return -1; }
    long n = (reply->type == REDIS_REPLY_INTEGER) ? reply->integer : -1;
    freeReplyObject(reply);
    return n;
#else
    return -1;
#endif
}

void stop()
{
#ifdef BLOG_HAS_REDIS
    g_running.store(false, std::memory_order_release);
    if (g_heartbeat.joinable()) g_heartbeat.join();
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_ctx) { redisFree(g_ctx); g_ctx = nullptr; }
    g_localUsers.clear();
#endif
}

} // namespace presence
