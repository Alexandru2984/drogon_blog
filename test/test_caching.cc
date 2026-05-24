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

// /posts (collection): 200 must carry ETag + Cache-Control, and a
// replay with the captured ETag in If-None-Match must return 304
// with an empty body.
DROGON_TEST(Caching_ListReturnsEtagAnd304)
{
    auto client = HttpClient::newHttpClient(testBaseUrl());

    auto req1 = HttpRequest::newHttpRequest();
    req1->setMethod(Get);
    req1->setPath("/posts");

    client->sendRequest(req1,
        [TEST_CTX, client](ReqResult r, const HttpResponsePtr& resp) {
            REQUIRE(r == ReqResult::Ok);
            REQUIRE(resp->getStatusCode() == k200OK);

            const std::string etag = resp->getHeader("ETag");
            const std::string cc   = resp->getHeader("Cache-Control");
            REQUIRE(!etag.empty());
            CHECK(etag.rfind("W/\"", 0) == 0);   // weak ETag
            CHECK(cc.find("must-revalidate") != std::string::npos);

            auto req2 = HttpRequest::newHttpRequest();
            req2->setMethod(Get);
            req2->setPath("/posts");
            req2->addHeader("If-None-Match", etag);

            client->sendRequest(req2,
                [TEST_CTX, etag](ReqResult r2, const HttpResponsePtr& resp2) {
                    REQUIRE(r2 == ReqResult::Ok);
                    CHECK(resp2->getStatusCode() == k304NotModified);
                    CHECK(resp2->getHeader("ETag") == etag);
                    CHECK(resp2->getBody().empty());
                });
        });
}

// /posts/{id}: same shape as the collection test, plus verify that
// modifying the post yields a *different* ETag (the conditional GET
// must miss after the write).
DROGON_TEST(Caching_PostDetailEtagBumpsOnUpdate)
{
    auto client = HttpClient::newHttpClient(testBaseUrl());
    const std::string username = "cache_" + uniqueSuffix();
    const std::string password = "cache-password-123";

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
          [TEST_CTX, client](ReqResult r2, const HttpResponsePtr& resp2) {
            REQUIRE(r2 == ReqResult::Ok);
            REQUIRE(resp2->getStatusCode() == k200OK);

            // Create a post under this session.
            Json::Value post;
            post["title"]   = "etag-test";
            post["content"] = "first body";
            auto postReq = HttpRequest::newHttpJsonRequest(post);
            postReq->setMethod(Post);
            postReq->setPath("/posts");
            for (const auto& [name, c] : resp2->getCookies()) {
                postReq->addCookie(name, c.value());
                if (name == "csrf_token") {
                    postReq->addHeader("X-CSRF-Token", c.value());
                }
            }

            client->sendRequest(postReq,
              [TEST_CTX, client, cookies=resp2->getCookies()]
              (ReqResult r3, const HttpResponsePtr& resp3) {
                REQUIRE(r3 == ReqResult::Ok);
                REQUIRE(resp3->getStatusCode() == k201Created);
                auto created = resp3->getJsonObject();
                REQUIRE(created);
                const int postId = (*created)["post"]["id"].asInt();

                // GET /posts/{id} — capture original ETag.
                auto getReq = HttpRequest::newHttpRequest();
                getReq->setMethod(Get);
                getReq->setPath("/posts/" + std::to_string(postId));

                client->sendRequest(getReq,
                  [TEST_CTX, client, cookies, postId]
                  (ReqResult r4, const HttpResponsePtr& resp4) {
                    REQUIRE(r4 == ReqResult::Ok);
                    REQUIRE(resp4->getStatusCode() == k200OK);
                    const std::string firstEtag = resp4->getHeader("ETag");
                    REQUIRE(!firstEtag.empty());

                    // Replay with If-None-Match → 304.
                    auto cond = HttpRequest::newHttpRequest();
                    cond->setMethod(Get);
                    cond->setPath("/posts/" + std::to_string(postId));
                    cond->addHeader("If-None-Match", firstEtag);
                    client->sendRequest(cond,
                      [TEST_CTX, client, cookies, postId, firstEtag]
                      (ReqResult r5, const HttpResponsePtr& resp5) {
                        REQUIRE(r5 == ReqResult::Ok);
                        CHECK(resp5->getStatusCode() == k304NotModified);

                        // Update the post → updated_at trigger fires →
                        // GET must return 200 with a different ETag,
                        // and the old If-None-Match must miss.
                        Json::Value put;
                        put["title"]   = "etag-test (edited)";
                        put["content"] = "second body";
                        auto putReq = HttpRequest::newHttpJsonRequest(put);
                        putReq->setMethod(Put);
                        putReq->setPath("/posts/" + std::to_string(postId));
                        for (const auto& [name, c] : cookies) {
                            putReq->addCookie(name, c.value());
                            if (name == "csrf_token") {
                                putReq->addHeader("X-CSRF-Token", c.value());
                            }
                        }
                        client->sendRequest(putReq,
                          [TEST_CTX, client, postId, firstEtag]
                          (ReqResult r6, const HttpResponsePtr& resp6) {
                            REQUIRE(r6 == ReqResult::Ok);
                            CHECK(resp6->getStatusCode() == k200OK);

                            auto recheck = HttpRequest::newHttpRequest();
                            recheck->setMethod(Get);
                            recheck->setPath("/posts/" + std::to_string(postId));
                            recheck->addHeader("If-None-Match", firstEtag);
                            client->sendRequest(recheck,
                              [TEST_CTX, firstEtag]
                              (ReqResult r7, const HttpResponsePtr& resp7) {
                                REQUIRE(r7 == ReqResult::Ok);
                                CHECK(resp7->getStatusCode() == k200OK);
                                const std::string newEtag = resp7->getHeader("ETag");
                                CHECK(!newEtag.empty());
                                CHECK(newEtag != firstEtag);
                              });
                          });
                      });
                  });
              });
          });
      });
}

// /posts/search?q=… : same protocol, scoped per query string.
DROGON_TEST(Caching_SearchReturnsEtagAnd304)
{
    auto client = HttpClient::newHttpClient(testBaseUrl());

    auto req = HttpRequest::newHttpRequest();
    req->setMethod(Get);
    req->setPath("/posts/search?q=etag");

    client->sendRequest(req,
        [TEST_CTX, client](ReqResult r, const HttpResponsePtr& resp) {
            REQUIRE(r == ReqResult::Ok);
            REQUIRE(resp->getStatusCode() == k200OK);
            const std::string etag = resp->getHeader("ETag");
            REQUIRE(!etag.empty());

            auto req2 = HttpRequest::newHttpRequest();
            req2->setMethod(Get);
            req2->setPath("/posts/search?q=etag");
            req2->addHeader("If-None-Match", etag);
            client->sendRequest(req2,
                [TEST_CTX](ReqResult r2, const HttpResponsePtr& resp2) {
                    REQUIRE(r2 == ReqResult::Ok);
                    CHECK(resp2->getStatusCode() == k304NotModified);
                });
        });
}

// An If-None-Match that doesn't match the live tag must NOT short-circuit:
// the handler should still return 200 + body + the current ETag.
DROGON_TEST(Caching_StaleIfNoneMatchReturns200)
{
    auto client = HttpClient::newHttpClient(testBaseUrl());

    auto req = HttpRequest::newHttpRequest();
    req->setMethod(Get);
    req->setPath("/posts");
    req->addHeader("If-None-Match", "W/\"stale-etag-that-cannot-match\"");

    client->sendRequest(req,
        [TEST_CTX](ReqResult r, const HttpResponsePtr& resp) {
            REQUIRE(r == ReqResult::Ok);
            CHECK(resp->getStatusCode() == k200OK);
            CHECK(!resp->getHeader("ETag").empty());
        });
}
