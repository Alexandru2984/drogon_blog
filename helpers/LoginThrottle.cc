#include "LoginThrottle.h"

#include "EmailHelper.h"

#include <drogon/drogon.h>
#include <trantor/utils/Logger.h>

#include <cstdlib>

namespace login_throttle {

namespace {

bool disabled()
{
    // Shares the integration-test escape hatch with the in-memory limiter,
    // so a test run does not have to know about two separate switches.
    const char* v = std::getenv("BLOG_DISABLE_RATE_LIMIT");
    return v && std::string(v) == "1";
}

} // namespace

Decision check(int userId)
{
    if (disabled()) return {};

    try {
        auto db = drogon::app().getDbClient();
        // Compute the remaining window in SQL so the decision uses the
        // database's clock. Comparing an application timestamp against a
        // database one drifts, and the drift here would either throttle
        // early or expire the window late.
        const auto r = db->execSqlSync(
            // $2::int is not decoration. PostgreSQL infers a parameter's
            // type from its strongest usage context, and an untyped $2 in
            // arithmetic here — or worse, inside make_interval(secs => …)
            // below, whose parameter is double precision — gets inferred
            // as something wider than the int32 Drogon binds. The result
            // is a binary-protocol size mismatch that surfaces as an
            // exception, which this function then swallows: the throttle
            // silently never engages. Pin every parameter to the width
            // that is actually bound.
            "SELECT failed_login_count, "
            "       GREATEST(0, $2::int - EXTRACT(EPOCH FROM (NOW() - last_failed_login))::int) "
            "         AS remaining "
            "  FROM users WHERE id = $1",
            userId, kWindowSeconds);
        if (r.empty() || r[0]["failed_login_count"].isNull()) return {};

        const int count = r[0]["failed_login_count"].as<int>();
        if (count < kThreshold) return {};

        const int remaining = r[0]["remaining"].isNull()
                                ? 0 : r[0]["remaining"].as<int>();
        if (remaining <= 0) return {};

        Decision d;
        d.throttled         = true;
        d.retryAfterSeconds = remaining;
        return d;
    } catch (const std::exception& e) {
        // Fail open: a database hiccup must not lock everyone out of the
        // site. The per-IP limiter and the in-memory per-username bucket
        // are both still in force on this path.
        LOG_ERROR << "login throttle check failed: " << e.what();
        return {};
    }
}

void recordFailure(int userId,
                   const std::string& email,
                   const std::string& username)
{
    if (disabled()) return;

    try {
        auto db = drogon::app().getDbClient();
        // Reset the counter when the previous failure is older than the
        // window: consecutive failures are what matter, not a lifetime
        // total. Someone who mistypes once a month should never accumulate
        // their way into a throttle.
        const auto r = db->execSqlSync(
            "UPDATE users "
            "   SET failed_login_count = CASE "
            "         WHEN last_failed_login IS NULL "
            "           OR last_failed_login < NOW() - make_interval(secs => $2::int) "
            "         THEN 1 ELSE failed_login_count + 1 END, "
            "       last_failed_login = NOW() "
            " WHERE id = $1 "
            "RETURNING failed_login_count, throttle_notified_at",
            userId, kWindowSeconds);
        if (r.empty()) return;

        const int count = r[0]["failed_login_count"].as<int>();
        if (count != kThreshold) return;   // only on the crossing itself

        // Notify once per episode. throttle_notified_at is cleared on the
        // next successful login, so a later attack sends a fresh warning
        // while a sustained one does not become its own mail flood.
        const auto n = db->execSqlSync(
            "UPDATE users SET throttle_notified_at = NOW() "
            " WHERE id = $1 AND throttle_notified_at IS NULL "
            "RETURNING id",
            userId);
        if (n.empty()) return;

        LOG_WARN << "login throttle engaged for user " << userId;
        EmailHelper::sendLoginThrottleEmail(email, username, kWindowSeconds / 60);
    } catch (const std::exception& e) {
        LOG_ERROR << "login throttle record failed: " << e.what();
    }
}

void recordSuccess(int userId)
{
    if (disabled()) return;

    try {
        auto db = drogon::app().getDbClient();
        db->execSqlSync(
            "UPDATE users "
            "   SET failed_login_count = 0, "
            "       last_failed_login = NULL, "
            "       throttle_notified_at = NULL "
            " WHERE id = $1 AND (failed_login_count <> 0 "
            "                    OR throttle_notified_at IS NOT NULL)",
            userId);
    } catch (const std::exception& e) {
        LOG_ERROR << "login throttle reset failed: " << e.what();
    }
}

} // namespace login_throttle
