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

// The read paths run inside execSqlAsync result callbacks, which Drogon
// invokes on the database client's own loop threads. A synchronous query
// issued from there — for tags, for the view counter, for anything —
// blocks the very thread that has to drive the connection it is waiting
// for. One request gets away with it; enough concurrent ones park every
// loop on another loop and the pool stops dead, permanently, because the
// client is configured without a statement timeout.
//
// That is not a slowdown: /healthz keeps answering 200 while every request
// that touches the database hangs until the process is restarted, which is
// exactly how it reaches production unnoticed.
//
// This fires more requests at once than the suite gives the pool
// connections (4, see test_main.cc), so a reintroduction cannot fit in the
// pool and hide.
DROGON_TEST(Posts_ConcurrentFeedReadsDoNotDeadlock)
{
    constexpr int kConcurrent = 16;

    // The timeout is per request and it is what turns the failure into a
    // report: without it a deadlocked server leaves the callbacks pending
    // and the CI job sits there until the six-hour cap.
    constexpr double kDeadline = 15.0;

    // One client per request, deliberately. A single HttpClient sends
    // everything down one connection, and the server answers a connection's
    // requests in arrival order — sixteen requests on one socket are
    // sixteen sequential requests, which is precisely the shape that does
    // not deadlock. Sixteen clients are sixteen sockets.
    for (int i = 0; i < kConcurrent; ++i) {
        auto client = HttpClient::newHttpClient(testBaseUrl());
        auto req = HttpRequest::newHttpRequest();
        req->setMethod(Get);
        req->setPath("/posts");
        client->sendRequest(req,
            [TEST_CTX, client](ReqResult r, const HttpResponsePtr& resp) {
                // ReqResult::Timeout here is the deadlock: the request was
                // accepted and then never answered.
                REQUIRE(r == ReqResult::Ok);
                CHECK(resp->getStatusCode() == k200OK);
            },
            kDeadline);
    }
}

// Same shape for the single-post read, which additionally records a view —
// a write — from inside its result callback.
DROGON_TEST(Posts_ConcurrentPostReadsDoNotDeadlock)
{
    constexpr int kConcurrent = 16;

    constexpr double kDeadline = 15.0;

    auto client = HttpClient::newHttpClient(testBaseUrl());

    const std::string username = "conc_" + uniqueSuffix();
    Json::Value reg;
    reg["username"] = username;
    reg["email"]    = username + "@example.test";
    reg["password"] = "concurrent-password-123";

    auto regReq = HttpRequest::newHttpJsonRequest(reg);
    regReq->setMethod(Post);
    regReq->setPath("/auth/register");

    client->sendRequest(regReq,
        [TEST_CTX, client, username](ReqResult r, const HttpResponsePtr& resp) {
            REQUIRE(r == ReqResult::Ok);
            REQUIRE(resp->getStatusCode() == k201Created);

            Json::Value login;
            login["username"] = username;
            login["password"] = "concurrent-password-123";
            auto loginReq = HttpRequest::newHttpJsonRequest(login);
            loginReq->setMethod(Post);
            loginReq->setPath("/auth/login");

            client->sendRequest(loginReq,
                [TEST_CTX, client](ReqResult r2, const HttpResponsePtr& resp2) {
                    REQUIRE(r2 == ReqResult::Ok);
                    REQUIRE(resp2->getStatusCode() == k200OK);

                    Json::Value post;
                    post["title"]   = "Concurrency";
                    post["content"] = "A post to read many times at once.";
                    post["tags"]    = Json::Value(Json::arrayValue);
                    post["tags"].append("concurrency");

                    auto createReq = HttpRequest::newHttpJsonRequest(post);
                    createReq->setMethod(Post);
                    createReq->setPath("/posts");
                    for (const auto& [name, c] : resp2->getCookies()) {
                        createReq->addCookie(name, c.value());
                        if (name == "csrf_token")
                            createReq->addHeader("X-CSRF-Token", c.value());
                    }

                    client->sendRequest(createReq,
                        [TEST_CTX, client](ReqResult r3, const HttpResponsePtr& resp3) {
                            REQUIRE(r3 == ReqResult::Ok);
                            REQUIRE(resp3->getStatusCode() == k201Created);

                            auto json = resp3->getJsonObject();
                            REQUIRE(json);
                            const auto id = (*json)["post"]["id"].asInt();

                            // One socket each, for the reason in the feed
                            // test above.
                            for (int i = 0; i < kConcurrent; ++i) {
                                auto reader = HttpClient::newHttpClient(testBaseUrl());
                                auto getReq = HttpRequest::newHttpRequest();
                                getReq->setMethod(Get);
                                getReq->setPath("/posts/" + std::to_string(id));
                                reader->sendRequest(getReq,
                                    [TEST_CTX, reader](ReqResult r4,
                                                       const HttpResponsePtr& resp4) {
                                        REQUIRE(r4 == ReqResult::Ok);
                                        CHECK(resp4->getStatusCode() == k200OK);
                                    },
                                    kDeadline);
                            }
                        });
                });
        });
}
