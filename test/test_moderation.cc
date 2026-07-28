#include <drogon/drogon.h>
#include <drogon/drogon_test.h>
#include <drogon/HttpClient.h>

#include "../helpers/Roles.h"

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
};

void promote(const std::string& username, const char* role)
{
    drogon::app().getDbClient()->execSqlSync(
        "UPDATE users SET role = $2 WHERE username = $1", username, role);
}

} // namespace

// Role ordering is what every gate depends on. An admin must satisfy a
// moderator requirement and a moderator must not satisfy an admin one;
// getting this backwards would either lock admins out of moderation or
// hand moderators the admin surface.
DROGON_TEST(Roles_OrderingIsMonotonic)
{
    using roles::Role;
    CHECK(roles::atLeast(Role::Admin,     Role::Moderator));
    CHECK(roles::atLeast(Role::Admin,     Role::Admin));
    CHECK(roles::atLeast(Role::Moderator, Role::Moderator));
    CHECK(roles::atLeast(Role::Moderator, Role::User));
    CHECK(!roles::atLeast(Role::Moderator, Role::Admin));
    CHECK(!roles::atLeast(Role::User,      Role::Moderator));

    // Anything unrecognised decodes to the least privilege. A decode that
    // guessed upward on unexpected input would be the wrong surprise.
    CHECK(roles::parse("admin")     == Role::Admin);
    CHECK(roles::parse("moderator") == Role::Moderator);
    CHECK(roles::parse("user")      == Role::User);
    CHECK(roles::parse("")          == Role::User);
    CHECK(roles::parse("ADMIN")     == Role::User);
    CHECK(roles::parse("superuser") == Role::User);
}

// An ordinary user probing the moderation surface must get 404, not 403.
// A 403 confirms the endpoint exists and is worth attacking; a 404 makes
// it indistinguishable from a route that was never there.
DROGON_TEST(Moderation_OrdinaryUserSeesNotFoundNotForbidden)
{
    const std::string u = "plain_" + uniq();
    const std::string p = "moderation-test-password-1";
    auto c = std::make_shared<Client>(testBaseUrl());

    Json::Value reg;
    reg["username"] = u;
    reg["email"]    = u + "@example.test";
    reg["password"] = p;

    Json::Value login;
    login["username"] = u;
    login["password"] = p;

    c->http->sendRequest(jsonPost("/auth/register", reg),
        [TEST_CTX, c, login](ReqResult, const HttpResponsePtr& r0) {
            REQUIRE(r0->getStatusCode() == k201Created);
            c->http->sendRequest(jsonPost("/auth/login", login),
                [TEST_CTX, c](ReqResult, const HttpResponsePtr& r1) {
                    REQUIRE(r1->getStatusCode() == k200OK);
                    c->absorb(r1);

                    auto req = HttpRequest::newHttpRequest();
                    req->setMethod(Get);
                    req->setPath("/admin/reports");
                    c->apply(req);
                    c->http->sendRequest(req,
                        [TEST_CTX](ReqResult, const HttpResponsePtr& r2) {
                            CHECK(r2->getStatusCode() == k404NotFound);
                        });
                });
        });
}

// Hiding a post must remove it from every read surface at once. The feed,
// the single-post route, search, the profile listing, the Atom feed and
// the share preview all read the same rows, and a predicate missing from
// any one of them makes moderation cosmetic.
DROGON_TEST(Moderation_HidingRemovesThePostEverywhere)
{
    const std::string u = "mod_" + uniq();
    const std::string p = "moderation-test-password-1";
    const std::string title = "quarantine-me-" + uniq();

    auto c = std::make_shared<Client>(testBaseUrl());

    Json::Value reg;
    reg["username"] = u;
    reg["email"]    = u + "@example.test";
    reg["password"] = p;

    Json::Value login;
    login["username"] = u;
    login["password"] = p;

    c->http->sendRequest(jsonPost("/auth/register", reg),
        [TEST_CTX, c, login, title, u](ReqResult, const HttpResponsePtr& r0) {
            REQUIRE(r0->getStatusCode() == k201Created);

            c->http->sendRequest(jsonPost("/auth/login", login),
                [TEST_CTX, c, title, u](ReqResult, const HttpResponsePtr& r1) {
                    REQUIRE(r1->getStatusCode() == k200OK);
                    c->absorb(r1);

                    Json::Value post;
                    post["title"]   = title;
                    post["content"] = "content that will be moderated";
                    auto pReq = jsonPost("/posts", post);
                    c->apply(pReq);

                    c->http->sendRequest(pReq,
                        [TEST_CTX, c, title, u](ReqResult, const HttpResponsePtr& r2) {
                            REQUIRE(r2->getStatusCode() == k201Created);
                            auto j = r2->getJsonObject();
                            REQUIRE(j);
                            const int postId = (*j)["post"]["id"].asInt();

                            // Visible before moderation.
                            auto get1 = HttpRequest::newHttpRequest();
                            get1->setMethod(Get);
                            get1->setPath("/posts/" + std::to_string(postId));
                            c->http->sendRequest(get1,
                                [TEST_CTX, c, postId, u](ReqResult, const HttpResponsePtr& r3) {
                                    REQUIRE(r3->getStatusCode() == k200OK);

                                    // Promote and hide.
                                    promote(u, "moderator");

                                    Json::Value body;
                                    body["reason"] = "test";
                                    auto hide = jsonPost(
                                        "/admin/posts/" + std::to_string(postId) + "/hide",
                                        body);
                                    c->apply(hide);

                                    c->http->sendRequest(hide,
                                        [TEST_CTX, c, postId](ReqResult, const HttpResponsePtr& r4) {
                                            REQUIRE(r4->getStatusCode() == k200OK);

                                            // Single post: gone.
                                            auto g = HttpRequest::newHttpRequest();
                                            g->setMethod(Get);
                                            g->setPath("/posts/" + std::to_string(postId));
                                            c->http->sendRequest(g,
                                                [TEST_CTX](ReqResult, const HttpResponsePtr& r5) {
                                                    CHECK(r5->getStatusCode() == k404NotFound);
                                                });

                                            // Feed: gone.
                                            auto f = HttpRequest::newHttpRequest();
                                            f->setMethod(Get);
                                            f->setPath("/posts?limit=50");
                                            c->http->sendRequest(f,
                                                [TEST_CTX, postId](ReqResult, const HttpResponsePtr& r6) {
                                                    REQUIRE(r6->getStatusCode() == k200OK);
                                                    auto jj = r6->getJsonObject();
                                                    REQUIRE(jj);
                                                    for (const auto& e : (*jj)["posts"]) {
                                                        CHECK(e["id"].asInt() != postId);
                                                    }
                                                });

                                            // Atom feed: gone.
                                            auto a = HttpRequest::newHttpRequest();
                                            a->setMethod(Get);
                                            a->setPath("/feed.xml");
                                            c->http->sendRequest(a,
                                                [TEST_CTX, postId](ReqResult, const HttpResponsePtr& r7) {
                                                    REQUIRE(r7->getStatusCode() == k200OK);
                                                    const std::string body{r7->getBody()};
                                                    CHECK(body.find("/posts/" + std::to_string(postId))
                                                          == std::string::npos);
                                                });

                                            // Share preview: gone.
                                            auto pv = HttpRequest::newHttpRequest();
                                            pv->setMethod(Get);
                                            pv->setPath("/preview/posts/" + std::to_string(postId));
                                            c->http->sendRequest(pv,
                                                [TEST_CTX](ReqResult, const HttpResponsePtr& r8) {
                                                    CHECK(r8->getStatusCode() == k404NotFound);
                                                });
                                        });
                                });
                        });
                });
        });
}

// A suspended account must not be able to write. The gate is central
// rather than per-handler, so this also covers endpoints added later.
DROGON_TEST(Moderation_SuspendedAccountCannotWriteButCanRead)
{
    const std::string u = "banned_" + uniq();
    const std::string p = "moderation-test-password-1";
    auto c = std::make_shared<Client>(testBaseUrl());

    Json::Value reg;
    reg["username"] = u;
    reg["email"]    = u + "@example.test";
    reg["password"] = p;

    Json::Value login;
    login["username"] = u;
    login["password"] = p;

    c->http->sendRequest(jsonPost("/auth/register", reg),
        [TEST_CTX, c, login, u](ReqResult, const HttpResponsePtr& r0) {
            REQUIRE(r0->getStatusCode() == k201Created);

            c->http->sendRequest(jsonPost("/auth/login", login),
                [TEST_CTX, c, u](ReqResult, const HttpResponsePtr& r1) {
                    REQUIRE(r1->getStatusCode() == k200OK);
                    c->absorb(r1);

                    // Suspend directly, then tell this process about it the
                    // way the pg_notify listener would. The listener is not
                    // running in the test harness, so without this the
                    // in-memory set would never learn about the ban.
                    auto db = drogon::app().getDbClient();
                    const auto row = db->execSqlSync(
                        "UPDATE users SET banned_until = NOW() + INTERVAL '1 day', "
                        "                 ban_reason = 'test' "
                        " WHERE username = $1 RETURNING id", u);
                    REQUIRE(!row.empty());
                    roles::onBanChangedNotification(row[0]["id"].as<int>(), true);

                    // Writing is refused…
                    Json::Value post;
                    post["title"]   = "should not appear";
                    post["content"] = "should not appear";
                    auto w = jsonPost("/posts", post);
                    c->apply(w);
                    c->http->sendRequest(w,
                        [TEST_CTX](ReqResult, const HttpResponsePtr& r2) {
                            CHECK(r2->getStatusCode() == k403Forbidden);
                        });

                    // …while reading still works. A ban means "you cannot
                    // contribute", not "you cannot see the site".
                    auto rd = HttpRequest::newHttpRequest();
                    rd->setMethod(Get);
                    rd->setPath("/posts");
                    c->apply(rd);
                    c->http->sendRequest(rd,
                        [TEST_CTX](ReqResult, const HttpResponsePtr& r3) {
                            CHECK(r3->getStatusCode() == k200OK);
                        });
                });
        });
}
