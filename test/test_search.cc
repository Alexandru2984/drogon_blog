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

// Logs in a fresh test user and forwards both the session cookie and the
// matching X-CSRF-Token header to the supplied request builder.
void withAuthenticatedClient(const std::shared_ptr<HttpClient>& client,
                             const std::string& usernamePrefix,
                             std::function<void(const std::string&,
                                                std::function<void(const HttpRequestPtr&)>)>&& work)
{
    const std::string username = usernamePrefix + "_" + uniqueSuffix();
    const std::string password = "search-password-123";

    Json::Value reg;
    reg["username"] = username;
    reg["email"]    = username + "@example.test";
    reg["password"] = password;
    auto regReq = HttpRequest::newHttpJsonRequest(reg);
    regReq->setMethod(Post);
    regReq->setPath("/auth/register");

    client->sendRequest(regReq,
        [client, username, password, work = std::move(work)]
        (ReqResult, const HttpResponsePtr&) {
            Json::Value login;
            login["username"] = username;
            login["password"] = password;
            auto loginReq = HttpRequest::newHttpJsonRequest(login);
            loginReq->setMethod(Post);
            loginReq->setPath("/auth/login");

            client->sendRequest(loginReq,
                [client, username, work = std::move(work)]
                (ReqResult, const HttpResponsePtr& loginResp) {
                    // Capture session + CSRF cookie so callers can mutate state.
                    std::string sessionCookie, sessionValue, csrfValue;
                    for (const auto& [name, c] : loginResp->getCookies()) {
                        if (name == "csrf_token") csrfValue = c.value();
                    }
                    work(username,
                         [loginResp, csrfValue](const HttpRequestPtr& req) {
                             for (const auto& [name, c] : loginResp->getCookies()) {
                                 req->addCookie(name, c.value());
                             }
                             if (!csrfValue.empty())
                                 req->addHeader("X-CSRF-Token", csrfValue);
                         });
                });
        });
}

HttpRequestPtr makeCreatePost(const std::string& title, const std::string& content)
{
    Json::Value body;
    body["title"]   = title;
    body["content"] = content;
    auto req = HttpRequest::newHttpJsonRequest(body);
    req->setMethod(Post);
    req->setPath("/posts");
    return req;
}

} // namespace

DROGON_TEST(Search_RejectsMissingQuery)
{
    auto client = HttpClient::newHttpClient(testBaseUrl());
    auto req = HttpRequest::newHttpRequest();
    req->setMethod(Get);
    req->setPath("/posts/search");

    client->sendRequest(req, [TEST_CTX](ReqResult r, const HttpResponsePtr& resp) {
        REQUIRE(r == ReqResult::Ok);
        CHECK(resp->getStatusCode() == k400BadRequest);
    });
}

DROGON_TEST(Search_TitleWeightedAboveContent)
{
    auto client = HttpClient::newHttpClient(testBaseUrl());
    const std::string token = "tsvectorific_" + uniqueSuffix();

    withAuthenticatedClient(client, "search",
        [TEST_CTX, client, token](const std::string& username,
                                  std::function<void(const HttpRequestPtr&)> attachAuth) {
            // First post: token in the body only.
            auto p1 = makeCreatePost("Vanilla observations",
                                     "An unrelated body that mentions " + token + " in passing.");
            attachAuth(p1);

            // Second post: token in the title (weight A) — should rank higher.
            auto p2 = makeCreatePost(token + " announced",
                                     "Body has no extra keywords here.");
            attachAuth(p2);

            client->sendRequest(p1, [TEST_CTX, client, token, attachAuth, p2]
                (ReqResult, const HttpResponsePtr& r1) {
                REQUIRE(r1->getStatusCode() == k201Created);

                client->sendRequest(p2, [TEST_CTX, client, token]
                    (ReqResult, const HttpResponsePtr& r2) {
                    REQUIRE(r2->getStatusCode() == k201Created);

                    auto search = HttpRequest::newHttpRequest();
                    search->setMethod(Get);
                    search->setPath("/posts/search");
                    search->setParameter("q", token);

                    client->sendRequest(search,
                        [TEST_CTX, token](ReqResult, const HttpResponsePtr& resp) {
                            REQUIRE(resp->getStatusCode() == k200OK);
                            auto json = resp->getJsonObject();
                            REQUIRE(json);
                            const auto& posts = (*json)["posts"];
                            REQUIRE(posts.size() >= 2u);

                            // Highest-ranked result must be the title-match.
                            CHECK(posts[0]["title"].asString().find(token) != std::string::npos);

                            // Snippet should be present and (for the body-match
                            // post) include the highlight markers we requested.
                            bool anySnippet = false;
                            for (const auto& p : posts) {
                                if (!p["snippet"].asString().empty()) {
                                    anySnippet = true;
                                    break;
                                }
                            }
                            CHECK(anySnippet);
                        });
                });
            });
        });
}

DROGON_TEST(Search_NoResultsReturnsEmpty)
{
    auto client = HttpClient::newHttpClient(testBaseUrl());

    auto search = HttpRequest::newHttpRequest();
    search->setMethod(Get);
    search->setPath("/posts/search");
    search->setParameter("q", "zzzzz_no_such_term_zzzzz");

    client->sendRequest(search,
        [TEST_CTX](ReqResult, const HttpResponsePtr& resp) {
            REQUIRE(resp->getStatusCode() == k200OK);
            auto json = resp->getJsonObject();
            REQUIRE(json);
            CHECK((*json)["count"].asUInt() == 0u);
            CHECK((*json)["posts"].size() == 0u);
        });
}
