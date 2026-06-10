#include "Ops.h"
#include "Metrics.h"

#include <drogon/drogon.h>
#include <drogon/orm/Exception.h>

#include <atomic>
#include <cstdlib>
#include <memory>
#include <string>

namespace ops {

namespace {

// Process-wide drain flag. Atomic because the readyz handler runs on a
// Drogon worker thread while the SIGTERM handler runs on the main loop.
// Default-init to false; beginDrain() flips it; no path resets it
// because draining is a terminal state for the process lifecycle.
std::atomic<bool> g_draining{false};

// Whether the request genuinely originated on this host (a local Prometheus
// scrape over loopback), as opposed to being forwarded by the reverse proxy.
//
// The peer address alone is NOT sufficient: behind nginx every request shows
// a 127.0.0.1 peer, so a naive loopback check would treat the whole internet
// as local and hand /metrics to anyone. nginx always injects X-Real-IP /
// X-Forwarded-For on proxied traffic, so the presence of either means the
// request came through the edge and must not get the tokenless bypass.
bool isDirectLocalRequest(const drogon::HttpRequestPtr& req)
{
    if (!req->getHeader("X-Forwarded-For").empty()) return false;
    if (!req->getHeader("X-Real-IP").empty())        return false;
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
            // Draining short-circuits the DB probe. The pod is alive
            // (liveness still passes) but we want the LB to remove us
            // from rotation immediately. Reporting a discriminator
            // status string lets a curious operator tell "shutting
            // down" apart from a real failure.
            if (g_draining.load(std::memory_order_acquire)) {
                Json::Value body;
                body["status"] = "draining";
                cb(jsonStatus(k503ServiceUnavailable, body));
                return;
            }
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
            } else if (!isDirectLocalRequest(req)) {
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

void beginDrain()
{
    g_draining.store(true, std::memory_order_release);
}

bool isDraining()
{
    return g_draining.load(std::memory_order_acquire);
}

} // namespace ops
