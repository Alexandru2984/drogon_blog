#include <drogon/drogon.h>
#include <drogon/drogon_test.h>
#include <drogon/HttpClient.h>

#include <chrono>
#include <cstdlib>
#include <string>

using namespace drogon;

namespace {

std::string testBaseUrl()
{
    const char* port = std::getenv("TEST_PORT");
    return std::string("http://127.0.0.1:") + (port && *port ? port : "18092");
}

std::string uniqueSuffix()
{
    auto ns = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::to_string(ns);
}

} // namespace

DROGON_TEST(Posts_AnonymousListReturnsEmpty)
{
    auto client = HttpClient::newHttpClient(testBaseUrl());
    auto req = HttpRequest::newHttpRequest();
    req->setMethod(Get);
    req->setPath("/posts");

    client->sendRequest(req, [TEST_CTX](ReqResult r, const HttpResponsePtr& resp) {
        REQUIRE(r == ReqResult::Ok);
        REQUIRE(resp->getStatusCode() == k200OK);
        auto json = resp->getJsonObject();
        REQUIRE(json);
        REQUIRE((*json).isMember("posts"));
        CHECK((*json)["posts"].isArray());
    });
}

DROGON_TEST(Posts_CreateRequiresAuth)
{
    auto client = HttpClient::newHttpClient(testBaseUrl());

    Json::Value body;
    body["title"]   = "anonymous attempt";
    body["content"] = "should be rejected";

    auto req = HttpRequest::newHttpJsonRequest(body);
    req->setMethod(Post);
    req->setPath("/posts");
    // Pass CSRF check so the auth guard is what rejects us. Otherwise the
    // synchronous advice would short-circuit with 403 before the handler.
    req->addCookie("csrf_token", "test-token");
    req->addHeader("X-CSRF-Token",  "test-token");

    client->sendRequest(req, [TEST_CTX](ReqResult r, const HttpResponsePtr& resp) {
        REQUIRE(r == ReqResult::Ok);
        CHECK(resp->getStatusCode() == k401Unauthorized);
    });
}

DROGON_TEST(Security_MissingCsrfBlocksMutation)
{
    auto client = HttpClient::newHttpClient(testBaseUrl());

    Json::Value body;
    body["title"]   = "no csrf";
    body["content"] = "should 403 before auth check";

    auto req = HttpRequest::newHttpJsonRequest(body);
    req->setMethod(Post);
    req->setPath("/posts");
    // No CSRF cookie / header → must be rejected by the advice.

    client->sendRequest(req, [TEST_CTX](ReqResult r, const HttpResponsePtr& resp) {
        REQUIRE(r == ReqResult::Ok);
        CHECK(resp->getStatusCode() == k403Forbidden);
    });
}

DROGON_TEST(Posts_FeedJoinIncludesAuthor)
{
    auto client = HttpClient::newHttpClient(testBaseUrl());
    const std::string username = "feed_" + uniqueSuffix();
    const std::string password = "feed-password-123";

    Json::Value reg;
    reg["username"] = username;
    reg["email"]    = username + "@example.test";
    reg["password"] = password;

    auto regReq = HttpRequest::newHttpJsonRequest(reg);
    regReq->setMethod(Post);
    regReq->setPath("/auth/register");

    client->sendRequest(regReq,
        [TEST_CTX, client, username, password](ReqResult r, const HttpResponsePtr& resp) {
            REQUIRE(r == ReqResult::Ok);
            REQUIRE(resp->getStatusCode() == k201Created);

            Json::Value login;
            login["username"] = username;
            login["password"] = password;
            auto loginReq = HttpRequest::newHttpJsonRequest(login);
            loginReq->setMethod(Post);
            loginReq->setPath("/auth/login");

            client->sendRequest(loginReq,
                [TEST_CTX, client, username](ReqResult r2, const HttpResponsePtr& resp2) {
                    REQUIRE(r2 == ReqResult::Ok);
                    REQUIRE(resp2->getStatusCode() == k200OK);

                    Json::Value post;
                    post["title"]   = "feed test " + username;
                    post["content"] = "exercised by Posts_FeedJoinIncludesAuthor";
                    auto postReq = HttpRequest::newHttpJsonRequest(post);
                    postReq->setMethod(Post);
                    postReq->setPath("/posts");

                    // Drogon's HttpClient does not auto-attach Set-Cookie values to
                    // subsequent requests, so propagate the session cookie manually,
                    // and echo the CSRF cookie in the X-CSRF-Token header.
                    for (const auto& [name, c] : resp2->getCookies()) {
                        postReq->addCookie(name, c.value());
                        if (name == "csrf_token") {
                            postReq->addHeader("X-CSRF-Token", c.value());
                        }
                    }

                    client->sendRequest(postReq,
                        [TEST_CTX, client, username](ReqResult r3, const HttpResponsePtr& resp3) {
                            REQUIRE(r3 == ReqResult::Ok);
                            REQUIRE(resp3->getStatusCode() == k201Created);

                            auto feedReq = HttpRequest::newHttpRequest();
                            feedReq->setMethod(Get);
                            feedReq->setPath("/posts");
                            client->sendRequest(feedReq,
                                [TEST_CTX, username](ReqResult r4, const HttpResponsePtr& resp4) {
                                    REQUIRE(r4 == ReqResult::Ok);
                                    REQUIRE(resp4->getStatusCode() == k200OK);

                                    auto json = resp4->getJsonObject();
                                    REQUIRE(json);
                                    REQUIRE((*json)["posts"].size() >= 1u);

                                    // The newest post should be ours and carry author info.
                                    bool found = false;
                                    for (const auto& p : (*json)["posts"]) {
                                        if (p.isMember("author") &&
                                            p["author"]["username"].asString() == username)
                                        {
                                            found = true;
                                            break;
                                        }
                                    }
                                    CHECK(found);
                                });
                        });
                });
        });
}
