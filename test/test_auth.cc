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

HttpRequestPtr jsonPost(const std::string& path, const Json::Value& body)
{
    auto req = HttpRequest::newHttpJsonRequest(body);
    req->setMethod(Post);
    req->setPath(path);
    return req;
}

} // namespace

DROGON_TEST(Auth_RegisterReturns201AndShortLatency)
{
    auto client = HttpClient::newHttpClient(testBaseUrl());
    const std::string username = "reg_" + uniqueSuffix();

    Json::Value body;
    body["username"] = username;
    body["email"]    = username + "@example.test";
    body["password"] = "register-password-123";

    auto t0 = std::chrono::steady_clock::now();
    client->sendRequest(jsonPost("/auth/register", body),
        [TEST_CTX, t0](ReqResult r, const HttpResponsePtr& resp) {
            REQUIRE(r == ReqResult::Ok);
            CHECK(resp->getStatusCode() == k201Created);

            // Async-mail invariant: the response must come back well before any
            // realistic SMTP round-trip could complete. 1.5 s is a generous bound.
            auto elapsed = std::chrono::steady_clock::now() - t0;
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
            CHECK(ms < 1500);
        });
}

DROGON_TEST(Auth_LoginRejectsWrongPassword)
{
    auto client = HttpClient::newHttpClient(testBaseUrl());
    const std::string username = "rej_" + uniqueSuffix();

    Json::Value reg;
    reg["username"] = username;
    reg["email"]    = username + "@example.test";
    reg["password"] = "the-real-password";

    client->sendRequest(jsonPost("/auth/register", reg),
        [TEST_CTX, client, username](ReqResult r, const HttpResponsePtr& resp) {
            REQUIRE(r == ReqResult::Ok);
            REQUIRE(resp->getStatusCode() == k201Created);

            Json::Value login;
            login["username"] = username;
            login["password"] = "totally-wrong";
            client->sendRequest(jsonPost("/auth/login", login),
                [TEST_CTX](ReqResult r2, const HttpResponsePtr& resp2) {
                    REQUIRE(r2 == ReqResult::Ok);
                    CHECK(resp2->getStatusCode() == k401Unauthorized);
                });
        });
}

DROGON_TEST(Auth_LoginAcceptsCorrectPassword)
{
    auto client = HttpClient::newHttpClient(testBaseUrl());
    const std::string username = "ok_" + uniqueSuffix();
    const std::string password = "ok-real-password-42";

    Json::Value reg;
    reg["username"] = username;
    reg["email"]    = username + "@example.test";
    reg["password"] = password;

    client->sendRequest(jsonPost("/auth/register", reg),
        [TEST_CTX, client, username, password](ReqResult r, const HttpResponsePtr& resp) {
            REQUIRE(r == ReqResult::Ok);
            REQUIRE(resp->getStatusCode() == k201Created);

            Json::Value login;
            login["username"] = username;
            login["password"] = password;
            client->sendRequest(jsonPost("/auth/login", login),
                [TEST_CTX, username](ReqResult r2, const HttpResponsePtr& resp2) {
                    REQUIRE(r2 == ReqResult::Ok);
                    REQUIRE(resp2->getStatusCode() == k200OK);

                    auto json = resp2->getJsonObject();
                    REQUIRE(json);
                    CHECK((*json)["user"]["username"].asString() == username);
                });
        });
}

DROGON_TEST(Auth_DuplicateUsernameReturns409)
{
    auto client = HttpClient::newHttpClient(testBaseUrl());
    const std::string username = "dup_" + uniqueSuffix();

    Json::Value body;
    body["username"] = username;
    body["email"]    = username + "@example.test";
    body["password"] = "dup-password-123";

    client->sendRequest(jsonPost("/auth/register", body),
        [TEST_CTX, client, body](ReqResult r, const HttpResponsePtr& resp) {
            REQUIRE(r == ReqResult::Ok);
            REQUIRE(resp->getStatusCode() == k201Created);

            client->sendRequest(jsonPost("/auth/register", body),
                [TEST_CTX](ReqResult r2, const HttpResponsePtr& resp2) {
                    REQUIRE(r2 == ReqResult::Ok);
                    CHECK(resp2->getStatusCode() == k409Conflict);
                });
        });
}
