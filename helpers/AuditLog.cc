#include "AuditLog.h"
#include "Security.h"

#include <drogon/drogon.h>
#include <drogon/orm/Exception.h>
#include <trantor/utils/Logger.h>

#include <utility>

namespace audit_log {

namespace {

constexpr const char* kReqIdAttr = "blog.req_id";

std::string reqIdOf(const drogon::HttpRequestPtr& req)
{
    auto attrs = req->attributes();
    if (!attrs->find(kReqIdAttr)) return {};
    return attrs->get<std::string>(kReqIdAttr);
}

std::string serialize(const Json::Value& v)
{
    Json::StreamWriterBuilder b;
    b["indentation"] = "";
    return Json::writeString(b, v);
}

} // namespace

void record(const drogon::HttpRequestPtr& req, Entry entry)
{
    auto db = drogon::app().getDbClient();
    if (!db) {
        LOG_ERROR << "audit_log::record('" << entry.action
                  << "'): no DB client; entry dropped";
        return;
    }

    const std::string ip       = security::clientIp(req);
    const std::string reqId    = reqIdOf(req);
    const std::string metaJson = serialize(entry.metadata);

    // The five nullable columns flow through libpq as NULLs when the
    // optional is empty. INSERT-only by design; this table has no
    // UPDATE/DELETE surface anywhere in the codebase.
    std::optional<std::string> reqIdOpt =
        reqId.empty() ? std::optional<std::string>{} : std::optional<std::string>{reqId};

    db->execSqlAsync(
        "INSERT INTO audit_log "
        "(actor_id, actor_ip, action, target_kind, target_id, metadata, req_id) "
        "VALUES ($1, $2, $3, $4, $5, $6::jsonb, $7)",
        [action = entry.action](const drogon::orm::Result&) {
            LOG_DEBUG << "audit_log: recorded " << action;
        },
        [action = entry.action](const drogon::orm::DrogonDbException& e) {
            LOG_ERROR << "audit_log: failed to record '" << action
                      << "': " << e.base().what();
        },
        entry.actor_id,
        ip,
        entry.action,
        entry.target_kind,
        entry.target_id,
        metaJson,
        reqIdOpt);
}

} // namespace audit_log
