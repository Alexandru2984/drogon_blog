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
    HttpRequestPtr post(const std::string& path) const
    {
        auto req = HttpRequest::newHttpRequest();
        req->setMethod(Post);
        req->setPath(path);
        apply(req);
        return req;
    }
    // Unlike is DELETE on the same /like resource, not POST /unlike.
    HttpRequestPtr del(const std::string& path) const
    {
        auto req = HttpRequest::newHttpRequest();
        req->setMethod(Delete);
        req->setPath(path);
        apply(req);
        return req;
    }
};

} // namespace

// GET /posts/{id}/likes has to report the viewer's own like, not just the
// total. Without it the client cannot draw a like control that knows its
// state — which is how the UI ended up with a Like button beside an Unlike
// button, neither of which could tell whether it applied.
//
// The whole lifecycle runs in one nested chain because the assertions are
// about the transitions: count and flag have to move together, and a second
// like must be a no-op rather than a second increment.
DROGON_TEST(Likes_ReportsViewersOwnLikeAndDeduplicates)
{
    const std::string u = "liker_" + uniq();
    const std::string p = "likes-test-password-1";
    auto c = std::make_shared<Client>(testBaseUrl());

    Json::Value reg;
    reg["username"] = u;
    reg["email"]    = u + "@example.test";
    reg["password"] = p;

    Json::Value login;
    login["username"] = u;
    login["password"] = p;

    Json::Value post;
    post["title"]   = "Likeable";
    post["content"] = "Something to like.";

    c->http->sendRequest(jsonPost("/auth/register", reg),
      [TEST_CTX, c, login, post](ReqResult, const HttpResponsePtr& r0) {
        REQUIRE(r0->getStatusCode() == k201Created);

        c->http->sendRequest(jsonPost("/auth/login", login),
          [TEST_CTX, c, post](ReqResult, const HttpResponsePtr& r1) {
            REQUIRE(r1->getStatusCode() == k200OK);
            c->absorb(r1);

            auto mk = jsonPost("/posts", post);
            c->apply(mk);
            c->http->sendRequest(mk,
              [TEST_CTX, c](ReqResult, const HttpResponsePtr& r2) {
                REQUIRE(r2->getStatusCode() == k201Created);
                auto body = r2->getJsonObject();
                REQUIRE(body != nullptr);
                const int postId = (*body)["post"]["id"].asInt();
                const std::string likesPath =
                    "/posts/" + std::to_string(postId) + "/likes";

                // Before liking: zero, and not liked by this viewer.
                c->http->sendRequest(c->get(likesPath),
                  [TEST_CTX, c, postId, likesPath](ReqResult, const HttpResponsePtr& r3) {
                    REQUIRE(r3->getStatusCode() == k200OK);
                    auto j = r3->getJsonObject();
                    REQUIRE(j != nullptr);
                    CHECK((*j)["likes_count"].asInt64() == 0);
                    // bool_or over no rows is NULL; it must surface as false,
                    // not as a null the client has to interpret.
                    CHECK((*j)["liked"].isBool());
                    CHECK((*j)["liked"].asBool() == false);

                    c->http->sendRequest(c->post("/posts/" + std::to_string(postId) + "/like"),
                      [TEST_CTX, c, postId, likesPath](ReqResult, const HttpResponsePtr& r4) {
                        // A fresh like is 200; the endpoint reserves 409 for
                        // the duplicate, asserted below.
                        REQUIRE(r4->getStatusCode() == k200OK);

                        c->http->sendRequest(c->get(likesPath),
                          [TEST_CTX, c, postId, likesPath](ReqResult, const HttpResponsePtr& r5) {
                            REQUIRE(r5->getStatusCode() == k200OK);
                            auto j = r5->getJsonObject();
                            REQUIRE(j != nullptr);
                            CHECK((*j)["likes_count"].asInt64() == 1);
                            CHECK((*j)["liked"].asBool() == true);

                            // Liking twice must not double-count. The UNIQUE
                            // constraint handles storage; this asserts the
                            // endpoint reports the duplicate and the count
                            // still agrees.
                            c->http->sendRequest(c->post("/posts/" + std::to_string(postId) + "/like"),
                              [TEST_CTX, c, postId, likesPath](ReqResult, const HttpResponsePtr& rdup) {
                                CHECK(rdup->getStatusCode() == k409Conflict);
                                c->http->sendRequest(c->get(likesPath),
                                  [TEST_CTX, c, postId, likesPath](ReqResult, const HttpResponsePtr& r6) {
                                    auto j = r6->getJsonObject();
                                    REQUIRE(j != nullptr);
                                    CHECK((*j)["likes_count"].asInt64() == 1);
                                    CHECK((*j)["liked"].asBool() == true);

                                    // And unliking returns to the start state.
                                    c->http->sendRequest(
                                      c->del("/posts/" + std::to_string(postId) + "/like"),
                                      [TEST_CTX, c, likesPath](ReqResult, const HttpResponsePtr& run) {
                                        REQUIRE(run->getStatusCode() == k200OK);
                                        c->http->sendRequest(c->get(likesPath),
                                          [TEST_CTX](ReqResult, const HttpResponsePtr& r7) {
                                            auto j = r7->getJsonObject();
                                            REQUIRE(j != nullptr);
                                            CHECK((*j)["likes_count"].asInt64() == 0);
                                            CHECK((*j)["liked"].asBool() == false);
                                          });
                                      });
                                  });
                              });
                          });
                      });
                  });
              });
          });
      });
}

// An anonymous reader gets the count and a false flag — never a 401, and
// never someone else's like state. The $2 bind is 0 for this case, so this
// also guards the "no user has id 0" assumption the query rests on.
DROGON_TEST(Likes_AnonymousReaderSeesCountButNeverLiked)
{
    const std::string u = "likeowner_" + uniq();
    const std::string p = "likes-test-password-1";
    auto owner = std::make_shared<Client>(testBaseUrl());
    // Separate client: no cookie jar, so genuinely anonymous.
    auto anon  = std::make_shared<Client>(testBaseUrl());

    Json::Value reg;
    reg["username"] = u;
    reg["email"]    = u + "@example.test";
    reg["password"] = p;

    Json::Value login;
    login["username"] = u;
    login["password"] = p;

    Json::Value post;
    post["title"]   = "Public counts";
    post["content"] = "Anyone may read the total.";

    owner->http->sendRequest(jsonPost("/auth/register", reg),
      [TEST_CTX, owner, anon, login, post](ReqResult, const HttpResponsePtr& r0) {
        REQUIRE(r0->getStatusCode() == k201Created);

        owner->http->sendRequest(jsonPost("/auth/login", login),
          [TEST_CTX, owner, anon, post](ReqResult, const HttpResponsePtr& r1) {
            REQUIRE(r1->getStatusCode() == k200OK);
            owner->absorb(r1);

            auto mk = jsonPost("/posts", post);
            owner->apply(mk);
            owner->http->sendRequest(mk,
              [TEST_CTX, owner, anon](ReqResult, const HttpResponsePtr& r2) {
                REQUIRE(r2->getStatusCode() == k201Created);
                auto body = r2->getJsonObject();
                REQUIRE(body != nullptr);
                const int postId = (*body)["post"]["id"].asInt();
                const std::string likesPath =
                    "/posts/" + std::to_string(postId) + "/likes";

                owner->http->sendRequest(owner->post("/posts/" + std::to_string(postId) + "/like"),
                  [TEST_CTX, anon, likesPath](ReqResult, const HttpResponsePtr& r3) {
                    REQUIRE(r3->getStatusCode() == k200OK);

                    auto req = HttpRequest::newHttpRequest();
                    req->setMethod(Get);
                    req->setPath(likesPath);
                    anon->http->sendRequest(req,
                      [TEST_CTX](ReqResult, const HttpResponsePtr& r4) {
                        REQUIRE(r4->getStatusCode() == k200OK);
                        auto j = r4->getJsonObject();
                        REQUIRE(j != nullptr);
                        CHECK((*j)["likes_count"].asInt64() == 1);
                        CHECK((*j)["liked"].asBool() == false);
                        // Per-viewer body: a shared cache must key on the
                        // cookie, and must not be allowed to store it.
                        CHECK(r4->getHeader("Vary").find("Cookie") != std::string::npos);
                        CHECK(r4->getHeader("Cache-Control").find("private")
                                != std::string::npos);
                      });
                  });
              });
          });
      });
}
