#include "Ops.h"
#include "Metrics.h"

#include <drogon/drogon.h>
#include <drogon/orm/Exception.h>

#include <cstdlib>
#include <memory>
#include <string>

namespace ops {

namespace {

bool isLoopbackPeer(const drogon::HttpRequestPtr& req)
{
    const auto ip = req->getPeerAddr().toIp();
    return ip == "127.0.0.1" || ip == "::1";
}

std::string envOr(const char* name, const char* fallback)
{
    const char* v = std::getenv(name);
    return (v && *v) ? std::string(v) : std::string(fallback);
}

drogon::HttpResponsePtr jsonStatus(drogon::HttpStatusCode code, const Json::Value& body)
{
    auto resp = drogon::HttpResponse::newHttpJsonResponse(body);
    resp->setStatusCode(code);
    return resp;
}

} // namespace

void install()
{
    using namespace drogon;

    // ---- /healthz ----
    app().registerHandler("/healthz",
        [](const HttpRequestPtr&,
           std::function<void(const HttpResponsePtr&)>&& cb) {
            Json::Value body;
            body["status"] = "ok";
            cb(jsonStatus(k200OK, body));
        },
        {Get});

    // ---- /readyz ----
    app().registerHandler("/readyz",
        [](const HttpRequestPtr&,
           std::function<void(const HttpResponsePtr&)>&& cb) {
            auto db = app().getDbClient();
            if (!db) {
                Json::Value body;
                body["status"] = "no db client";
                cb(jsonStatus(k503ServiceUnavailable, body));
                return;
            }
            db->execSqlAsync("SELECT 1",
                [cb](const orm::Result&) {
                    Json::Value body;
                    body["status"] = "ok";
                    cb(jsonStatus(k200OK, body));
                },
                [cb](const orm::DrogonDbException& e) {
                    Json::Value body;
                    body["status"] = "db unreachable";
                    body["error"]  = e.base().what();
                    cb(jsonStatus(k503ServiceUnavailable, body));
                });
        },
        {Get});

    // ---- /metrics ----
    // Gated by METRICS_TOKEN bearer auth when set, otherwise only available to
    // loopback peers (production deployments should set the token and front
    // /metrics with TLS through nginx).
    const std::string token = envOr("METRICS_TOKEN", "");

    app().registerHandler("/metrics",
        [token](const HttpRequestPtr& req,
                std::function<void(const HttpResponsePtr&)>&& cb) {
            if (!token.empty()) {
                const auto auth = req->getHeader("Authorization");
                const std::string expected = "Bearer " + token;
                if (auth != expected) {
                    auto r = HttpResponse::newHttpResponse();
                    r->setStatusCode(k401Unauthorized);
                    r->setBody("unauthorized\n");
                    r->setContentTypeCode(CT_TEXT_PLAIN);
                    cb(r);
                    return;
                }
            } else if (!isLoopbackPeer(req)) {
                auto r = HttpResponse::newHttpResponse();
                r->setStatusCode(k403Forbidden);
                r->setBody("metrics disabled for remote peers (set METRICS_TOKEN to enable)\n");
                r->setContentTypeCode(CT_TEXT_PLAIN);
                cb(r);
                return;
            }

            auto r = HttpResponse::newHttpResponse();
            r->setStatusCode(k200OK);
            r->setBody(metrics::renderPrometheus());
            r->setContentTypeString("text/plain; version=0.0.4; charset=utf-8");
            cb(r);
        },
        {Get});
}

} // namespace ops
