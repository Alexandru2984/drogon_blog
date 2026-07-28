#include "Roles.h"

#include <drogon/drogon.h>
#include <trantor/utils/Logger.h>

#include <shared_mutex>
#include <unordered_set>

namespace roles {

namespace {

// Currently-suspended user ids.
//
// Checked before every content mutation, so a database round-trip here
// would tax the write path to cover an event that happens a handful of
// times a year. Bans are rare and the set is tiny; it is loaded at startup
// and kept current by the user_ban_changed event on blog_event.
std::shared_mutex          g_bannedMu;
std::unordered_set<int>    g_banned;

// Paths exempt from the write gate. Logging out and completing a
// two-step login must keep working for a suspended account: the first so
// they are not stuck in a broken client, the second because /auth/login
// already refuses them and the pending state would otherwise linger.
bool exemptPath(const std::string& path)
{
    return path.rfind("/auth/", 0) == 0;
}

} // namespace

bool atLeast(Role have, Role need)
{
    return static_cast<int>(have) >= static_cast<int>(need);
}

Role parse(const std::string& s)
{
    if (s == "admin")     return Role::Admin;
    if (s == "moderator") return Role::Moderator;
    // Anything unrecognised falls back to the least privilege. The column
    // has a CHECK constraint so this should be unreachable, but a decode
    // that guessed *upward* on unexpected input would be the wrong kind of
    // surprise.
    return Role::User;
}

const char* name(Role r)
{
    switch (r) {
        case Role::Admin:     return "admin";
        case Role::Moderator: return "moderator";
        default:              return "user";
    }
}

std::optional<Role> of(const drogon::HttpRequestPtr& req)
{
    auto session = req->session();
    if (!session) return std::nullopt;
    auto userId = session->getOptional<int>("user_id");
    if (!userId) return std::nullopt;

    try {
        auto db = drogon::app().getDbClient();
        // Read the role from the database on each check rather than
        // caching it in the session. A session lives up to 14 days, so a
        // cached role would mean revoking someone's moderator rights does
        // not take effect until they happen to sign out — exactly when it
        // matters least.
        const auto r = db->execSqlSync(
            "SELECT role FROM users WHERE id = $1", *userId);
        if (r.empty()) return std::nullopt;
        return parse(r[0]["role"].as<std::string>());
    } catch (const std::exception& e) {
        LOG_ERROR << "role lookup failed: " << e.what();
        // Fail closed: an unreadable role is not a moderator.
        return Role::User;
    }
}

drogon::HttpResponsePtr require(const drogon::HttpRequestPtr& req, Role need)
{
    auto have = of(req);

    if (!have) {
        Json::Value body;
        body["error"] = "Not authenticated";
        auto resp = drogon::HttpResponse::newHttpJsonResponse(body);
        resp->setStatusCode(drogon::k401Unauthorized);
        return resp;
    }
    if (atLeast(*have, need)) return nullptr;

    // 404, not 403. A 403 tells an ordinary user that the endpoint exists
    // and is worth probing; a 404 makes the whole moderation surface
    // indistinguishable from routes that were never there.
    Json::Value body;
    body["error"] = "Not found";
    auto resp = drogon::HttpResponse::newHttpJsonResponse(body);
    resp->setStatusCode(drogon::k404NotFound);
    return resp;
}

BanState banStateOf(int userId)
{
    BanState st;
    try {
        auto db = drogon::app().getDbClient();
        const auto r = db->execSqlSync(
            "SELECT banned_until, ban_reason, "
            "       (banned_until IS NOT NULL AND banned_until > NOW()) AS active "
            "  FROM users WHERE id = $1",
            userId);
        if (r.empty()) return st;
        st.banned = !r[0]["active"].isNull() && r[0]["active"].as<bool>();
        if (st.banned) {
            st.until  = r[0]["banned_until"].as<std::string>();
            st.reason = r[0]["ban_reason"].isNull()
                            ? std::string{} : r[0]["ban_reason"].as<std::string>();
        }
    } catch (const std::exception& e) {
        LOG_ERROR << "ban lookup failed: " << e.what();
        // Fail open on a database error: a hiccup should not lock the
        // whole site out of posting. The ban still applies on the next
        // request that reads successfully.
    }
    return st;
}

drogon::HttpResponsePtr blockIfBanned(int userId)
{
    const auto st = banStateOf(userId);
    if (!st.banned) return nullptr;

    Json::Value body;
    // Say why and until when. A ban the user cannot understand reads as a
    // broken site, and generates support noise rather than reflection.
    body["error"]  = "Your account is suspended";
    body["until"]  = st.until;
    body["reason"] = st.reason;
    auto resp = drogon::HttpResponse::newHttpJsonResponse(body);
    resp->setStatusCode(drogon::k403Forbidden);
    return resp;
}


bool isBanned(int userId)
{
    std::shared_lock<std::shared_mutex> lk(g_bannedMu);
    return g_banned.count(userId) != 0;
}

void onBanChangedNotification(int userId, bool banned)
{
    std::unique_lock<std::shared_mutex> lk(g_bannedMu);
    if (banned) g_banned.insert(userId);
    else        g_banned.erase(userId);
}

void install()
{
    // Deferred onto the loop: Drogon builds its DB client pool inside
    // run(), so getDbClient() at install time asserts on an empty map.
    // Same shape as flags::install() and sessions::install().
    drogon::app().getLoop()->queueInLoop([] {
        try {
            auto db = drogon::app().getDbClient();
            if (!db) return;
            const auto r = db->execSqlSync(
                "SELECT id FROM users "
                " WHERE banned_until IS NOT NULL AND banned_until > NOW()");
            std::unique_lock<std::shared_mutex> lk(g_bannedMu);
            for (const auto& row : r) g_banned.insert(row["id"].as<int>());
            if (!r.empty()) {
                LOG_INFO << "roles: " << r.size() << " suspended account(s) loaded";
            }
        } catch (const std::exception& e) {
            LOG_ERROR << "roles: could not load bans: " << e.what();
        }
    });

    // Intercepting form of the pre-routing advice, because this one has to
    // be able to answer the request. Pre-routing rather than a sync advice
    // for the same reason as the session check: Drogon resolves the
    // session *after* sync advices run, so a sync advice would always see
    // a null session and never fire.
    drogon::app().registerPreRoutingAdvice(
        [](const drogon::HttpRequestPtr& req,
           drogon::AdviceCallback&&      respond,
           drogon::AdviceChainCallback&& proceed) {
            const auto method = req->getMethod();
            if (method == drogon::Get || method == drogon::Head ||
                method == drogon::Options || exemptPath(req->getPath()))
            {
                proceed();
                return;
            }

            auto session = req->session();
            if (!session) { proceed(); return; }
            auto userId = session->getOptional<int>("user_id");
            if (!userId || !isBanned(*userId)) { proceed(); return; }

            // Reading is still allowed — this only gates writes. A ban is
            // "you cannot contribute", not "you cannot see the site".
            Json::Value body;
            body["error"] = "Your account is suspended";
            auto resp = drogon::HttpResponse::newHttpJsonResponse(body);
            resp->setStatusCode(drogon::k403Forbidden);
            respond(resp);
        });
}

} // namespace roles
