#include <drogon/drogon.h>
#include <drogon/drogon_test.h>
#include <drogon/HttpClient.h>

#include <chrono>
#include <cstdlib>
#include <memory>
#include <string>

using namespace drogon;

namespace {

std::string testBaseUrl()
{
    const char* port = std::getenv("TEST_PORT");
    return std::string("http://127.0.0.1:") + (port && *port ? port : "18092");
}

std::string uniq()
{
    return std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
}

HttpRequestPtr jsonPost(const std::string& path, const Json::Value& body)
{
    auto req = HttpRequest::newHttpJsonRequest(body);
    req->setMethod(Post);
    req->setPath(path);
    return req;
}

struct Client {
    HttpClientPtr http;
    std::string   session;
    std::string   csrf;

    explicit Client(const std::string& base)
        : http(HttpClient::newHttpClient(base)) {}

    void absorb(const HttpResponsePtr& r)
    {
        for (const auto& c : r->getCookies()) {
            if (c.second.key() == "JSESSIONID") session = c.second.value();
            if (c.second.key() == "csrf_token") csrf    = c.second.value();
        }
    }
    void apply(const HttpRequestPtr& req) const
    {
        if (!session.empty()) req->addCookie("JSESSIONID", session);
        if (!csrf.empty()) {
            req->addCookie("csrf_token", csrf);
            req->addHeader("X-CSRF-Token", csrf);
        }
    }
    HttpRequestPtr get(const std::string& path) const
    {
        auto req = HttpRequest::newHttpRequest();
        req->setMethod(Get);
        req->setPath(path);
        apply(req);
        return req;
    }
    HttpRequestPtr json(const std::string& path, HttpMethod m,
                        const Json::Value& body) const
    {
        auto req = HttpRequest::newHttpJsonRequest(body);
        req->setMethod(m);
        req->setPath(path);
        apply(req);
        return req;
    }
};

// The document every preview assertion below is made against: a heading a
// table of contents would pick up, a fenced block with a language the
// highlighter recognises, and a link, so the excerpt has markdown syntax to
// strip.
const char* kDoc =
    "# Indexes\n\n"
    "Some **bold** words and a [link](https://example.com/deep/path).\n\n"
    "## Partial indexes\n\n"
    "```sql\n"
    "CREATE INDEX ON posts (id) WHERE published_at IS NOT NULL;\n"
    "```\n";

} // namespace

// The whole reason the preview is rendered server-side: it must be the same
// bytes publishing produces. A second, client-side markdown renderer would
// be one round trip cheaper and would quietly disagree — and every place it
// disagreed, the preview would be lying about what the post will look like.
DROGON_TEST(Preview_MatchesWhatPublishingProduces)
{
    const std::string u = "prev_" + uniq();
    const std::string p = "editor-test-password-1";
    auto c = std::make_shared<Client>(testBaseUrl());

    Json::Value reg; reg["username"]=u; reg["email"]=u+"@example.test"; reg["password"]=p;
    Json::Value log; log["username"]=u; log["password"]=p;

    c->http->sendRequest(jsonPost("/auth/register", reg),
      [TEST_CTX, c, log](ReqResult, const HttpResponsePtr& r0) {
        REQUIRE(r0->getStatusCode() == k201Created);
        c->http->sendRequest(jsonPost("/auth/login", log),
          [TEST_CTX, c](ReqResult, const HttpResponsePtr& r1) {
            REQUIRE(r1->getStatusCode() == k200OK);
            c->absorb(r1);

            Json::Value body; body["content"] = kDoc;
            c->http->sendRequest(c->json("/posts/preview", Post, body),
              [TEST_CTX, c](ReqResult, const HttpResponsePtr& r2) {
                REQUIRE(r2->getStatusCode() == k200OK);
                auto j = r2->getJsonObject();
                REQUIRE(j != nullptr);

                const std::string html = (*j)["content_html"].asString();
                CHECK(html.find("<h1")  != std::string::npos);
                CHECK(html.find("<h2")  != std::string::npos);
                // cmark-gfm puts the fence's info string on <pre lang=…>,
                // which is exactly what lib/highlight.ts reads.
                CHECK(html.find("lang=\"sql\"") != std::string::npos);
                CHECK((*j)["reading_minutes"].asInt() >= 1);
                CHECK(!(*j)["excerpt"].asString().empty());
                // An unpublished draft body: never stored by a shared cache.
                CHECK(r2->getHeader("Cache-Control").find("no-store")
                        != std::string::npos);

                // Now publish the same source and compare.
                Json::Value post;
                post["title"]   = "Indexes";
                post["content"] = kDoc;
                c->http->sendRequest(c->json("/posts", Post, post),
                  [TEST_CTX, c, html](ReqResult, const HttpResponsePtr& r3) {
                    REQUIRE(r3->getStatusCode() == k201Created);
                    auto jp = r3->getJsonObject();
                    REQUIRE(jp != nullptr);
                    const int id = (*jp)["post"]["id"].asInt();

                    c->http->sendRequest(c->get("/posts/" + std::to_string(id)),
                      [TEST_CTX, html](ReqResult, const HttpResponsePtr& r4) {
                        REQUIRE(r4->getStatusCode() == k200OK);
                        auto jd = r4->getJsonObject();
                        REQUIRE(jd != nullptr);
                        CHECK((*jd)["content_html"].asString() == html);
                      });
                  });
              });
          });
      });
}

// The endpoint renders caller-supplied markdown on a worker pool. Without a
// size ceiling it is a CPU amplifier that needs no post to be created and
// leaves no row behind to clean up.
DROGON_TEST(Preview_RejectsOversizedContentAndAnonymousCallers)
{
    // No session and no CSRF cookie. 403 rather than 401 is correct and
    // deliberate: the CSRF guard is a filter that runs ahead of the
    // controller, so it answers before the session is ever consulted.
    auto anon = std::make_shared<Client>(testBaseUrl());
    Json::Value small; small["content"] = "hello";
    anon->http->sendRequest(jsonPost("/posts/preview", small),
      [TEST_CTX](ReqResult, const HttpResponsePtr& r) {
        CHECK((r->getStatusCode() == k403Forbidden ||
               r->getStatusCode() == k401Unauthorized));
      });

    const std::string u = "prevbig_" + uniq();
    const std::string p = "editor-test-password-1";
    auto c = std::make_shared<Client>(testBaseUrl());

    Json::Value reg; reg["username"]=u; reg["email"]=u+"@example.test"; reg["password"]=p;
    Json::Value log; log["username"]=u; log["password"]=p;

    c->http->sendRequest(jsonPost("/auth/register", reg),
      [TEST_CTX, c, log](ReqResult, const HttpResponsePtr& r0) {
        REQUIRE(r0->getStatusCode() == k201Created);
        c->http->sendRequest(jsonPost("/auth/login", log),
          [TEST_CTX, c](ReqResult, const HttpResponsePtr& r1) {
            REQUIRE(r1->getStatusCode() == k200OK);
            c->absorb(r1);

            // One byte over the 100 KiB the create endpoint enforces, so
            // the two limits cannot drift apart unnoticed.
            Json::Value big;
            big["content"] = std::string(100 * 1024 + 1, 'a');
            c->http->sendRequest(c->json("/posts/preview", Post, big),
              [TEST_CTX](ReqResult, const HttpResponsePtr& r2) {
                CHECK(r2->getStatusCode() == k413RequestEntityTooLarge);
              });
          });
      });
}

// "More like this" is a claim about the two posts, so it is built from tags
// they actually share. A draft must never be offered as a suggestion — that
// would leak an unpublished title through a back door the post endpoint
// itself closes.
DROGON_TEST(Related_UsesSharedTagsAndHidesDrafts)
{
    const std::string u = "rel_" + uniq();
    const std::string p = "editor-test-password-1";
    // Unique per run so a previous run's rows cannot satisfy the assertions.
    const std::string tagShared = "reltag" + uniq();
    const std::string tagOther  = "othertag" + uniq();
    auto c = std::make_shared<Client>(testBaseUrl());

    Json::Value reg; reg["username"]=u; reg["email"]=u+"@example.test"; reg["password"]=p;
    Json::Value log; log["username"]=u; log["password"]=p;

    c->http->sendRequest(jsonPost("/auth/register", reg),
      [TEST_CTX, c, log, tagShared, tagOther](ReqResult, const HttpResponsePtr& r0) {
        REQUIRE(r0->getStatusCode() == k201Created);
        c->http->sendRequest(jsonPost("/auth/login", log),
          [TEST_CTX, c, tagShared, tagOther](ReqResult, const HttpResponsePtr& r1) {
            REQUIRE(r1->getStatusCode() == k200OK);
            c->absorb(r1);

            auto make = [c](const std::string& title,
                            const std::vector<std::string>& tags,
                            bool draft) {
                Json::Value body;
                body["title"]   = title;
                body["content"] = "Body of " + title + ".";
                body["draft"]   = draft;
                Json::Value arr(Json::arrayValue);
                for (const auto& t : tags) arr.append(t);
                body["tags"] = arr;
                return c->json("/posts", Post, body);
            };

            // The post being read: tagged shared + other.
            c->http->sendRequest(make("Origin", {tagShared, tagOther}, false),
              [TEST_CTX, c, tagShared, tagOther, make](ReqResult, const HttpResponsePtr& ra) {
                REQUIRE(ra->getStatusCode() == k201Created);
                const int originId = (*ra->getJsonObject())["post"]["id"].asInt();

                // A published neighbour sharing one tag: must be offered.
                c->http->sendRequest(make("Neighbour", {tagShared}, false),
                  [TEST_CTX, c, originId, tagShared, make](ReqResult, const HttpResponsePtr& rb) {
                    REQUIRE(rb->getStatusCode() == k201Created);
                    const int neighbourId = (*rb->getJsonObject())["post"]["id"].asInt();

                    // A draft sharing the same tag: must not be.
                    c->http->sendRequest(make("Secret", {tagShared}, true),
                      [TEST_CTX, c, originId, neighbourId, make](ReqResult, const HttpResponsePtr& rc) {
                        REQUIRE(rc->getStatusCode() == k201Created);
                        const int draftId = (*rc->getJsonObject())["post"]["id"].asInt();

                        c->http->sendRequest(
                          c->get("/posts/" + std::to_string(originId) + "/related"),
                          [TEST_CTX, c, originId, neighbourId, draftId, make](
                              ReqResult, const HttpResponsePtr& rd) {
                            REQUIRE(rd->getStatusCode() == k200OK);
                            auto j = rd->getJsonObject();
                            REQUIRE(j != nullptr);

                            bool sawNeighbour = false, sawDraft = false, sawSelf = false;
                            for (const auto& item : (*j)["posts"]) {
                                const int id = item["id"].asInt();
                                if (id == neighbourId) {
                                    sawNeighbour = true;
                                    CHECK(item["shared_tags"].asInt() >= 1);
                                }
                                if (id == draftId)  sawDraft = true;
                                if (id == originId) sawSelf  = true;
                            }
                            CHECK(sawNeighbour == true);
                            CHECK(sawDraft     == false);
                            // A post is not related to itself, however many
                            // tags it shares with itself.
                            CHECK(sawSelf      == false);

                            // A post with no tags has no shared tags, and
                            // that is reported as an empty list rather than
                            // padded out with whatever was published last.
                            c->http->sendRequest(make("Untagged", {}, false),
                              [TEST_CTX, c](ReqResult, const HttpResponsePtr& re) {
                                REQUIRE(re->getStatusCode() == k201Created);
                                const int lonelyId = (*re->getJsonObject())["post"]["id"].asInt();
                                c->http->sendRequest(
                                  c->get("/posts/" + std::to_string(lonelyId) + "/related"),
                                  [TEST_CTX](ReqResult, const HttpResponsePtr& rf) {
                                    REQUIRE(rf->getStatusCode() == k200OK);
                                    auto jj = rf->getJsonObject();
                                    REQUIRE(jj != nullptr);
                                    CHECK((*jj)["posts"].size() == 0u);
                                  });
                              });
                          });
                      });
                  });
              });
          });
      });
}
