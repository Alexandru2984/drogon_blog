#include "FlagsController.h"
#include "../helpers/Flags.h"

#include <trantor/utils/Logger.h>

namespace {

// Anonymous callers bucket against user_id=0. Session-bound flags
// (rare for a blog — most flags are global rollouts) would need a
// dedicated key, but the deterministic-by-userId model gives us
// the common "ramp this feature to 30 % of users" pattern for free.
int callerUserId(const drogon::HttpRequestPtr& req)
{
    auto session = req->session();
    if (!session) return 0;
    auto u = session->getOptional<int>("user_id");
    return u ? *u : 0;
}

} // namespace

void FlagsController::get(const HttpRequestPtr& req,
                          std::function<void(const HttpResponsePtr&)>&& cb,
                          std::string key)
{
    // Path params are URL-decoded by Drogon, so a key containing a
    // colon or hyphen comes through fine. We bound the length to
    // avoid memory-amplification on hostile inputs — flag keys are
    // operator-controlled and short by convention.
    if (key.empty() || key.size() > 128) {
        Json::Value ret; ret["error"] = "Invalid flag key";
        auto resp = HttpResponse::newHttpJsonResponse(ret);
        resp->setStatusCode(k400BadRequest);
        cb(resp);
        return;
    }
    const int userId = callerUserId(req);
    const auto val = flags::lookup(key, userId);

    Json::Value ret;
    ret["key"] = key;
    if (val.has_value()) {
        ret["known"]   = true;
        ret["enabled"] = *val;
    } else {
        // Unknown keys are surfaced explicitly so a caller can tell
        // "the flag is off" from "you misspelled it". Either way the
        // flag evaluates to false in production code paths
        // (flags::isEnabled() fails closed).
        ret["known"]   = false;
        ret["enabled"] = false;
    }
    auto resp = HttpResponse::newHttpJsonResponse(ret);
    // Flags change rarely. A short TTL lets the SPA's composable
    // de-duplicate rapid-fire lookups without holding a stale
    // decision for long; mutations propagate via PG NOTIFY within
    // ms, so the cache window is the dominant staleness.
    resp->addHeader("Cache-Control", "private, max-age=30, must-revalidate");
    cb(resp);
}

void FlagsController::list(const HttpRequestPtr& req,
                           std::function<void(const HttpResponsePtr&)>&& cb)
{
    const int userId = callerUserId(req);
    const auto results = flags::evaluateAll(userId);

    Json::Value ret;
    ret["flags"] = Json::Value(Json::arrayValue);
    for (const auto& r : results) {
        Json::Value e;
        e["key"]     = r.key;
        e["enabled"] = r.enabled;
        ret["flags"].append(e);
    }
    auto resp = HttpResponse::newHttpJsonResponse(ret);
    resp->addHeader("Cache-Control", "private, max-age=30, must-revalidate");
    cb(resp);
}
