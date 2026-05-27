#include <drogon/drogon.h>
#include <json/json.h>
#include <sodium.h>
#include "helpers/AccessLog.h"
#include "helpers/EmailHelper.h"
#include "helpers/ImageProcessor.h"
#include "helpers/Markdown.h"
#include "helpers/ApiDocs.h"
#include "helpers/Ops.h"
#include "helpers/Flags.h"
#include "helpers/PgListener.h"
#include "helpers/Presence.h"
#include "helpers/Sentry.h"
#include "helpers/GrpcServer.h"
#include "helpers/PublicPages.h"
#include "helpers/Security.h"
#include "controllers/MessageWebSocket.h"

#include <chrono>
#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_set>
#include <cstdlib>
#include <cctype>

namespace {

void loadEnvFile(const std::string& path)
{
    std::ifstream file(path);
    if (!file.is_open()) return;

    std::string line;
    while (std::getline(file, line))
    {
        if (line.empty() || line[0] == '#') continue;
        auto pos = line.find('=');
        if (pos == std::string::npos) continue;

        std::string key   = line.substr(0, pos);
        std::string value = line.substr(pos + 1);

        auto trim = [](std::string& s) {
            while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
            while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))  s.pop_back();
        };
        trim(key); trim(value);

        if (value.size() >= 2 &&
            ((value.front() == '"' && value.back() == '"') ||
             (value.front() == '\'' && value.back() == '\'')))
        {
            value = value.substr(1, value.size() - 2);
        }

        if (!key.empty()) setenv(key.c_str(), value.c_str(), 1);
    }
}

// Expands ${VAR_NAME} occurrences using the current process env.
std::string expandEnv(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); )
    {
        if (s[i] == '$' && i + 1 < s.size() && s[i + 1] == '{')
        {
            auto end = s.find('}', i + 2);
            if (end != std::string::npos)
            {
                std::string name = s.substr(i + 2, end - (i + 2));
                const char* v = std::getenv(name.c_str());
                if (v) out.append(v);
                i = end + 1;
                continue;
            }
        }
        out.push_back(s[i++]);
    }
    return out;
}

} // namespace

int main()
{
    std::cout << "Starting Drogon blog server..." << std::endl;

    loadEnvFile("./.env");

    // Listener bind address is env-driven so the same config.json works in
    // every deployment: bare-metal/systemd behind nginx wants loopback
    // (set BLOG_LISTEN_ADDR=127.0.0.1 in .env), while a container MUST bind
    // 0.0.0.0 or Docker's port publishing can't reach it. Default 0.0.0.0 so
    // Docker/CI work out of the box; overwrite=0 keeps any value the env
    // (.env / systemd / compose) already provided. config.json references
    // ${BLOG_LISTEN_ADDR} in its listeners block.
    setenv("BLOG_LISTEN_ADDR", "0.0.0.0", 0);

    std::ifstream cfg("./config.json");
    if (!cfg)
    {
        std::cerr << "Error: config.json not found" << std::endl;
        return 1;
    }
    std::stringstream raw;
    raw << cfg.rdbuf();
    std::string expanded = expandEnv(raw.str());

    Json::CharReaderBuilder builder;
    builder["allowComments"]            = true;
    builder["allowTrailingCommas"]      = true;
    builder["allowSingleQuotes"]        = true;
    builder["allowSpecialFloats"]       = true;
    builder["skipBom"]                  = true;

    Json::Value cfgJson;
    std::string errs;
    std::istringstream iss(expanded);
    if (!Json::parseFromStream(builder, iss, &cfgJson, &errs))
    {
        std::cerr << "Failed to parse config.json: " << errs << std::endl;
        return 1;
    }

    if (sodium_init() < 0)
    {
        std::cerr << "libsodium init failed" << std::endl;
        return 1;
    }

    if (!image::initLibrary())
    {
        std::cerr << "libvips init failed" << std::endl;
        return 1;
    }

    // Register cmark-gfm core extensions (tables, strikethrough, autolinks,
    // tasklists). Safe to call before app().run().
    markdown::initOnce();

    try
    {
        drogon::app().loadConfigJson(cfgJson);
        std::cout << "Config loaded successfully!" << std::endl;
    }
    catch (const std::exception& e)
    {
        std::cerr << "Error loading config: " << e.what() << std::endl;
        return 1;
    }

    EmailHelper::start();
    presence::install();
    sentry::install();
    rpc::install();
    flags::install();

    // Cross-process WebSocket fan-out: a dedicated libpq connection LISTENs
    // on `blog_event` and routes every notification into the in-process
    // MessageWebSocket hub. New comments / new messages are produced by
    // INSERT-time triggers in the DB so this code path works the same on a
    // single node and across a horizontally-scaled fleet.
    pglisten::start("blog_event",
        [](const std::string& /*channel*/, const std::string& payload) {
            Json::CharReaderBuilder rb;
            Json::Value             root;
            std::string             errs;
            std::istringstream      iss(payload);
            if (!Json::parseFromStream(rb, iss, &root, &errs)) return;

            const std::string kind = root.get("kind", "").asString();
            if (kind == "message" &&
                root["sender_id"].isInt() && root["receiver_id"].isInt())
            {
                MessageWebSocket::pushNewMessage(
                    root["receiver_id"].asInt(),
                    root["sender_id"].asInt(),
                    root);
            }
            else if (kind == "comment" && root["post_id"].isInt())
            {
                MessageWebSocket::pushNewComment(
                    root["post_id"].asInt(), root);
            }
            else if (kind == "flag_changed")
            {
                // Migration 0007 routes feature_flags mutations
                // through the same channel. The whole cache reloads
                // — the table is small, and a partial update would
                // double the surface for stale reads on delete.
                flags::reload();
            }
        });

    drogon::app().getLoop()->runOnQuit([] {
        pglisten::stop();
        EmailHelper::stop();
        presence::stop();
        sentry::stop();
        rpc::stop();
        image::shutdownLibrary();
    });

    // Rate limiting, CSRF (double-submit), and response security headers.
    security::registerAdvices();

    // Structured JSON access log + request-ID propagation + metrics ingestion.
    access_log::install();

    // /healthz, /readyz, /metrics.
    ops::install();

    // /api/openapi.yaml + /api/redoc.standalone.js + /api/docs.
    api_docs::install();

    // /feed.xml (Atom 1.0) + /preview/posts/{id} (OpenGraph/Twitter cards).
    public_pages::install(
        std::getenv("BLOG_SITE_ORIGIN")
            ? std::getenv("BLOG_SITE_ORIGIN")
            : "https://blog.micutu.com");

    // Graceful shutdown.
    //
    // K8s rolling deploy / systemd restart sends SIGTERM. The default
    // Drogon handler just calls app().quit() — which yanks in-flight
    // requests and slams open WebSocket connections shut. Override so:
    //
    //   1. ops::beginDrain() flips /readyz to 503 immediately. Upstream
    //      load balancers stop sending us new traffic on their next probe.
    //   2. A short sleep gives the LB time to remove us from the pool
    //      *and* lets the K8s PreStop hook (chart/.../deployment.yaml)
    //      cover the kube-proxy propagation window. 2 s is a deliberate
    //      lower bound — the PreStop hook does the heavier waiting.
    //   3. WebSocket connections get a clean close-frame so the SPA's
    //      reconnect logic kicks in immediately instead of waiting for
    //      a TCP read timeout.
    //   4. app().quit() finally drains in-flight HTTP and stops the loop;
    //      the loop's runOnQuit then drains PgListener + EmailHelper.
    //
    // SIGINT (Ctrl-C in dev) takes the same path so tests + manual runs
    // observe the same shutdown semantics as production.
    const auto shutdownHandler = [] {
        LOG_INFO << "shutdown signal received — draining";
        ops::beginDrain();
        std::this_thread::sleep_for(std::chrono::seconds(2));
        MessageWebSocket::shutdownAll();
        drogon::app().quit();
    };
    drogon::app().setTermSignalHandler(shutdownHandler);
    drogon::app().setIntSignalHandler(shutdownHandler);

    std::cout << "Drogon listening (see config for port)..." << std::endl;
    drogon::app().run();
    return 0;
}
