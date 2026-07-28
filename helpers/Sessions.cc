#include "Sessions.h"

#include "Security.h"

#include <drogon/drogon.h>
#include <trantor/utils/Logger.h>

#include <chrono>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <unordered_set>

namespace sessions {

namespace {

constexpr const char* kSidKey = "sid";

// Revoked sids, checked before every handler.
//
// A database lookup per authenticated request would be the obvious
// implementation and the wrong one: it puts a query on the hot path of
// every single hit to pay for an event that happens a handful of times a
// year. The set is bounded in practice — an entry only has to outlive the
// session it kills, and Drogon's in-memory store is wiped on restart, so
// the set starts empty each boot and only grows by actual revocations.
std::shared_mutex               g_revokedMu;
std::unordered_set<std::string> g_revoked;

// Throttle for last_seen_at writes. Updating on every request would turn a
// read-only page view into a write; once every few minutes is enough to
// make "last active" meaningful in the UI.
constexpr auto kLastSeenInterval = std::chrono::minutes(5);

std::mutex g_seenMu;
std::unordered_map<std::string, std::chrono::steady_clock::time_point> g_lastSeen;

bool isRevoked(const std::string& sid)
{
    std::shared_lock<std::shared_mutex> lk(g_revokedMu);
    return g_revoked.count(sid) != 0;
}

void markRevokedLocally(const std::string& sid)
{
    std::unique_lock<std::shared_mutex> lk(g_revokedMu);
    g_revoked.insert(sid);
}

// Best-effort, fire-and-forget: a failed last_seen update is not worth
// failing a request over, and the next one will retry anyway.
void touch(const std::string& sid, const drogon::HttpRequestPtr& req)
{
    {
        std::lock_guard<std::mutex> lk(g_seenMu);
        const auto now = std::chrono::steady_clock::now();
        auto it = g_lastSeen.find(sid);
        if (it != g_lastSeen.end() && now - it->second < kLastSeenInterval) return;
        g_lastSeen[sid] = now;
    }

    auto db = drogon::app().getDbClient();
    if (!db) return;
    db->execSqlAsync(
        "UPDATE user_sessions SET last_seen_at = NOW(), ip = $2 "
        " WHERE sid = $1 AND revoked_at IS NULL",
        [](const drogon::orm::Result&) {},
        [](const drogon::orm::DrogonDbException& e) {
            LOG_DEBUG << "session touch failed: " << e.base().what();
        },
        sid, security::clientIp(req));
}

} // namespace

void install()
{
    // Drogon's session store lives in process memory, so every session that
    // existed before this boot is already gone. Retire the rows that
    // describe them, otherwise the session list shows entries the user
    // cannot possibly be using and "revoke" on them does nothing visible.
    //
    // Deferred onto the loop rather than run here: Drogon builds its DB
    // client pool inside run(), so calling getDbClient() at install time
    // trips an assertion on an empty map. Same reason and same shape as
    // flags::install(). Queueing it means it fires as soon as the loop
    // starts pumping, by which point the pool exists.
    drogon::app().getLoop()->queueInLoop([] {
        try {
            auto db = drogon::app().getDbClient();
            if (!db) {
                LOG_WARN << "sessions: no db client; skipping reconciliation";
                return;
            }
            const auto r = db->execSqlSync(
                "UPDATE user_sessions "
                "   SET revoked_at = NOW(), revoked_reason = 'restart' "
                " WHERE revoked_at IS NULL "
                "RETURNING sid");
            if (!r.empty()) {
                LOG_INFO << "sessions: retired " << r.size()
                         << " row(s) left over from the previous process";
            }
        } catch (const std::exception& e) {
            // A blog that cannot reconcile its session table should still
            // serve traffic; the consequence is a stale list, not a
            // security hole — those sessions really are dead either way.
            LOG_ERROR << "sessions: reconciliation failed: " << e.what();
        }
    });

    // Pre-routing, NOT a sync advice.
    //
    // Drogon's pipeline runs sync advices before it resolves the session:
    // HttpServer.cc calls passSyncAdvices(), then findSessionForRequest(),
    // then passPreRoutingAdvices(). A sync advice therefore always sees a
    // null session — the first version of this check was one, and it
    // silently never fired, so revoked sessions kept working.
    //
    // The observer overload is the right one here: this needs to inspect
    // and mutate the session, never to answer the request itself, and
    // Drogon documents it as the cheaper of the two.
    drogon::app().registerPreRoutingAdvice(
        [](const drogon::HttpRequestPtr& req) {
            auto session = req->session();
            if (!session) return;

            auto sid = session->getOptional<std::string>(kSidKey);
            if (!sid || sid->empty()) return;

            if (isRevoked(*sid)) {
                // Drop the whole session rather than answering 401 here:
                // every downstream handler already treats a missing user_id
                // as unauthenticated, so this needs no per-handler support,
                // and a revoked session should not keep any other state
                // either.
                session->clear();
                return;
            }

            touch(*sid, req);
        });
}

std::string begin(const drogon::HttpRequestPtr& req, int userId)
{
    const std::string sid = security::randomToken(18);

    auto session = req->session();
    if (session) session->insert(kSidKey, sid);

    try {
        auto db = drogon::app().getDbClient();
        // User-Agent is stored to make the session list legible ("Firefox on
        // Linux"), and truncated because it is attacker-controlled text that
        // ends up rendered in that list.
        std::string ua = req->getHeader("User-Agent");
        if (ua.size() > 255) ua.resize(255);

        db->execSqlSync(
            "INSERT INTO user_sessions (sid, user_id, ip, user_agent) "
            "VALUES ($1, $2, $3, $4)",
            sid, userId, security::clientIp(req), ua);
    } catch (const std::exception& e) {
        // The session itself is already valid at this point; failing the
        // login because bookkeeping failed would be the worse outcome. The
        // cost is one session missing from the list.
        LOG_ERROR << "could not record session: " << e.what();
    }
    return sid;
}

std::optional<std::string> currentSid(const drogon::HttpRequestPtr& req)
{
    auto session = req->session();
    if (!session) return std::nullopt;
    auto sid = session->getOptional<std::string>(kSidKey);
    if (!sid || sid->empty()) return std::nullopt;
    return sid;
}

bool revoke(int userId, const std::string& sid, const std::string& reason)
{
    try {
        auto db = drogon::app().getDbClient();
        // Scoped by user_id so a caller cannot revoke someone else's session
        // by guessing a sid — the WHERE clause is the authorization check.
        const auto r = db->execSqlSync(
            "UPDATE user_sessions "
            "   SET revoked_at = NOW(), revoked_reason = $3 "
            " WHERE sid = $1 AND user_id = $2 AND revoked_at IS NULL "
            "RETURNING sid",
            sid, userId, reason);
        if (r.empty()) return false;
    } catch (const std::exception& e) {
        LOG_ERROR << "session revoke failed: " << e.what();
        return false;
    }

    // Apply locally straight away rather than waiting for our own
    // notification to come back around: the user who just clicked "sign out
    // this device" should see it take effect on the very next request, and
    // the listener is asynchronous.
    markRevokedLocally(sid);
    return true;
}

int revokeOthers(int userId, const std::string& keepSid,
                 const std::string& reason)
{
    try {
        auto db = drogon::app().getDbClient();
        const auto r = db->execSqlSync(
            "UPDATE user_sessions "
            "   SET revoked_at = NOW(), revoked_reason = $3 "
            " WHERE user_id = $1 AND revoked_at IS NULL AND sid <> $2 "
            "RETURNING sid",
            userId, keepSid, reason);
        for (const auto& row : r) {
            markRevokedLocally(row["sid"].as<std::string>());
        }
        return static_cast<int>(r.size());
    } catch (const std::exception& e) {
        LOG_ERROR << "bulk session revoke failed: " << e.what();
        return 0;
    }
}

Json::Value list(int userId, const std::string& current)
{
    Json::Value arr(Json::arrayValue);
    try {
        auto db = drogon::app().getDbClient();
        const auto r = db->execSqlSync(
            "SELECT sid, created_at, last_seen_at, ip, user_agent "
            "  FROM user_sessions "
            " WHERE user_id = $1 AND revoked_at IS NULL "
            " ORDER BY last_seen_at DESC",
            userId);
        for (const auto& row : r) {
            const auto sid = row["sid"].as<std::string>();
            Json::Value e;
            e["sid"]          = sid;
            e["created_at"]   = row["created_at"].as<std::string>();
            e["last_seen_at"] = row["last_seen_at"].as<std::string>();
            e["ip"]           = row["ip"].isNull()
                                    ? "" : row["ip"].as<std::string>();
            e["user_agent"]   = row["user_agent"].isNull()
                                    ? "" : row["user_agent"].as<std::string>();
            e["current"]      = (sid == current);
            arr.append(e);
        }
    } catch (const std::exception& e) {
        LOG_ERROR << "session list failed: " << e.what();
    }
    return arr;
}

void onRevokedNotification(const std::string& sid)
{
    if (sid.empty()) return;
    markRevokedLocally(sid);
}

} // namespace sessions
