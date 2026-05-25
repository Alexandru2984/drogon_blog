#include "ApiDocs.h"

#include <drogon/drogon.h>
#include <trantor/utils/Logger.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace api_docs {

namespace {

// Resolve a file under openapi/ relative to two candidate roots: the
// repo CWD (dev + systemd: WorkingDirectory=/home/micu/drogon_blog) and
// the container WORKDIR (/app). On miss, return empty so the handler
// can fail with a clear 503 instead of pretending the route exists.
std::string findFile(const char* relative)
{
    namespace fs = std::filesystem;
    for (const auto* root : {".", "/app"}) {
        auto p = fs::path(root) / relative;
        if (fs::exists(p)) return p.string();
    }
    return {};
}

std::string readAll(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return std::move(ss).str();
}

// Static HTML — single same-origin script + a `<redoc>` element keyed
// to the spec endpoint. Keeping the page minimal (no app shell, no SPA
// mount) avoids the Vue router catching the route and keeps the byte
// budget tiny. Same-origin script means the existing CSP `script-src 'self'`
// covers us without widening for a CDN.
constexpr std::string_view kDocsHtml = R"HTML(<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Drogon Blog — API</title>
  <link rel="icon" href="data:,">
  <style>body { margin: 0; padding: 0; font-family: -apple-system, system-ui, sans-serif; }</style>
</head>
<body>
  <redoc spec-url="/api/openapi.yaml" hide-download-button="true"></redoc>
  <script src="/api/redoc.standalone.js"></script>
</body>
</html>
)HTML";

// Serve a file from disk with a fixed Content-Type. Centralised because
// the spec + the Redoc bundle handlers are otherwise identical.
void serveFile(const std::string&             path,
               const std::string&             contentType,
               int                            maxAgeSeconds,
               std::function<void(const drogon::HttpResponsePtr&)>&& cb)
{
    using namespace drogon;
    if (path.empty()) {
        auto r = HttpResponse::newHttpJsonResponse(Json::Value("API docs asset not deployed"));
        r->setStatusCode(k503ServiceUnavailable);
        cb(r);
        return;
    }
    std::string body = readAll(path);
    if (body.empty()) {
        auto r = HttpResponse::newHttpJsonResponse(Json::Value("API docs asset unreadable"));
        r->setStatusCode(k503ServiceUnavailable);
        cb(r);
        return;
    }
    auto resp = HttpResponse::newHttpResponse();
    resp->setBody(std::move(body));
    resp->setContentTypeString(contentType);
    char buf[64];
    std::snprintf(buf, sizeof(buf),
        "public, max-age=%d, must-revalidate", maxAgeSeconds);
    resp->addHeader("Cache-Control", buf);
    cb(resp);
}

} // namespace

void install()
{
    using namespace drogon;

    // Paths are resolved once at install() and captured into the
    // handlers — avoids the syscall on every request. Trade-off: the
    // operator can't drop a new spec into place without a restart.
    // That's the right trade-off: spec changes ship as part of the
    // binary's source tree.
    const std::string specPath = findFile("openapi/blog.openapi.yaml");
    const std::string jsPath   = findFile("openapi/redoc.standalone.js");

    if (specPath.empty()) {
        LOG_WARN << "OpenAPI spec not found on disk; /api/openapi.yaml will 503.";
    } else {
        LOG_INFO << "Serving OpenAPI spec from " << specPath;
    }
    if (jsPath.empty()) {
        LOG_WARN << "Redoc bundle not found on disk; /api/docs will load a broken page.";
    } else {
        LOG_INFO << "Serving Redoc bundle from " << jsPath;
    }

    // GET /api/openapi.yaml — short cache window keeps the spec close
    // to fresh after a deploy; intermediaries are still allowed to hold
    // it briefly. text/yaml isn't IANA-registered but Redoc / Scalar /
    // curl all accept application/yaml.
    app().registerHandler(
        "/api/openapi.yaml",
        [specPath](const HttpRequestPtr&,
                   std::function<void(const HttpResponsePtr&)>&& cb) {
            serveFile(specPath, "application/yaml; charset=utf-8", 60, std::move(cb));
        },
        {Get});

    // GET /api/redoc.standalone.js — long cache window because the
    // bundle is content-addressed by version (we'd bump the path
    // before changing contents, see the openapi/ README).
    app().registerHandler(
        "/api/redoc.standalone.js",
        [jsPath](const HttpRequestPtr&,
                 std::function<void(const HttpResponsePtr&)>&& cb) {
            serveFile(jsPath,
                      "application/javascript; charset=utf-8",
                      86400 * 7,
                      std::move(cb));
        },
        {Get});

    // GET /api/docs — the viewer page.
    app().registerHandler(
        "/api/docs",
        [](const HttpRequestPtr&,
           std::function<void(const HttpResponsePtr&)>&& cb) {
            auto resp = HttpResponse::newHttpResponse();
            resp->setBody(std::string(kDocsHtml));
            resp->setContentTypeString("text/html; charset=utf-8");
            resp->addHeader("Cache-Control", "public, max-age=300, must-revalidate");
            cb(resp);
        },
        {Get});
}

} // namespace api_docs
