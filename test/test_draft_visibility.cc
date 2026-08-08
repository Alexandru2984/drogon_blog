#include <drogon/drogon.h>
#include <drogon/drogon_test.h>
#include <drogon/HttpClient.h>

#include <chrono>
#include <cstdlib>
#include <functional>
#include <memory>
#include <string>

using namespace drogon;

// An unpublished post is visible to its author and to nobody else.
//
// getPost, getAllPosts, getPostsByTag, getRelatedPosts and getMyDrafts all
// carried the `published_at IS NOT NULL` half of that rule from the day
// drafts were added; searchPosts, getUserPosts, the comment endpoints, the
// like endpoint and addBookmark did not. Each of those was reachable without
// a session, so the leak was to anonymous callers: full `content` from
// /posts/user/{id}, a ts_headline fragment from /posts/search, and a
// 404-vs-success split everywhere else that answered "yes, that draft
// exists" to anyone willing to count upwards.
//
// These tests pin the whole rule rather than the two worst cases, because
// the pattern of the bug was one query at a time forgetting a clause its
// neighbours had.

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

// What a caller needs to act as a signed-in user: the id the server assigned
// them, and a way to stamp a request with their session + CSRF pair.
struct Author {
    int                                     userId = 0;
    std::function<void(const HttpRequestPtr&)> attachAuth;
};

// Registers a user, logs them in, and hands the callback an Author. Mirrors
// the helper in test_search.cc but also resolves the numeric user id, which
// these tests need to address /posts/user/{id}.
void withAuthor(const std::shared_ptr<HttpClient>& client,
                const std::string& prefix,
                std::function<void(const Author&)>&& work)
{
    const std::string username = prefix + "_" + uniqueSuffix();
    const std::string password = "draft-visibility-123";

    Json::Value reg;
    reg["username"] = username;
    reg["email"]    = username + "@example.test";
    reg["password"] = password;
    auto regReq = HttpRequest::newHttpJsonRequest(reg);
    regReq->setMethod(Post);
    regReq->setPath("/auth/register");

    client->sendRequest(regReq,
        [client, username, password, work = std::move(work)]
        (ReqResult, const HttpResponsePtr&) mutable {
            Json::Value login;
            login["username"] = username;
            login["password"] = password;
            auto loginReq = HttpRequest::newHttpJsonRequest(login);
            loginReq->setMethod(Post);
            loginReq->setPath("/auth/login");

            client->sendRequest(loginReq,
                [client, work = std::move(work)]
                (ReqResult, const HttpResponsePtr& loginResp) mutable {
                    std::string csrf;
                    for (const auto& [name, c] : loginResp->getCookies()) {
                        if (name == "csrf_token") csrf = c.value();
                    }
                    auto attach = [loginResp, csrf](const HttpRequestPtr& r) {
                        for (const auto& [name, c] : loginResp->getCookies()) {
                            r->addCookie(name, c.value());
                        }
                        if (!csrf.empty()) r->addHeader("X-CSRF-Token", csrf);
                    };

                    // /auth/me is the only place the id is handed back.
                    auto me = HttpRequest::newHttpRequest();
                    me->setMethod(Get);
                    me->setPath("/auth/me");
                    attach(me);

                    client->sendRequest(me,
                        [attach, work = std::move(work)]
                        (ReqResult, const HttpResponsePtr& meResp) {
                            Author a;
                            a.attachAuth = attach;
                            if (auto j = meResp->getJsonObject()) {
                                a.userId = (*j)["id"].asInt();
                            }
                            work(a);
                        });
                });
        });
}

// Creates a post. `draft` chooses whether published_at is stamped.
HttpRequestPtr makePost(const std::string& title,
                        const std::string& content,
                        bool draft)
{
    Json::Value body;
    body["title"]   = title;
    body["content"] = content;
    body["draft"]   = draft;
    auto req = HttpRequest::newHttpJsonRequest(body);
    req->setMethod(Post);
    req->setPath("/posts");
    return req;
}

// A request with no cookies at all — the anonymous reader every one of these
// endpoints was answering.
HttpRequestPtr anonGet(const std::string& path)
{
    auto req = HttpRequest::newHttpRequest();
    req->setMethod(Get);
    req->setPath(path);
    return req;
}

} // namespace

DROGON_TEST(Drafts_AreNotReturnedBySearch)
{
    auto client = HttpClient::newHttpClient(testBaseUrl());
    // A term that cannot collide with any other row in the test database.
    const std::string token = "draftleak_" + uniqueSuffix();

    withAuthor(client, "draftsearch",
        [TEST_CTX, client, token](const Author& a) {
            auto create = makePost("Unpublished " + token,
                                   "The body also mentions " + token + ".",
                                   /*draft=*/true);
            a.attachAuth(create);

            client->sendRequest(create,
                [TEST_CTX, client, token](ReqResult, const HttpResponsePtr& c) {
                    REQUIRE(c->getStatusCode() == k201Created);

                    auto search = anonGet("/posts/search");
                    search->setParameter("q", token);
                    client->sendRequest(search,
                        [TEST_CTX](ReqResult r, const HttpResponsePtr& resp) {
                            REQUIRE(r == ReqResult::Ok);
                            REQUIRE(resp->getStatusCode() == k200OK);
                            auto json = resp->getJsonObject();
                            REQUIRE(json);
                            // Not "no snippet" — no row at all. The title and
                            // the author leak just as much as the body does.
                            CHECK((*json)["count"].asUInt() == 0u);
                            CHECK((*json)["posts"].size() == 0u);
                        });
                });
        });
}

DROGON_TEST(Drafts_AppearInSearchOncePublished)
{
    // The negative test above passes just as well against a search that is
    // broken outright, so pin the other direction too.
    auto client = HttpClient::newHttpClient(testBaseUrl());
    const std::string token = "draftpub_" + uniqueSuffix();

    withAuthor(client, "draftpub",
        [TEST_CTX, client, token](const Author& a) {
            auto create = makePost("Published " + token,
                                   "Body mentioning " + token + ".",
                                   /*draft=*/false);
            a.attachAuth(create);

            client->sendRequest(create,
                [TEST_CTX, client, token](ReqResult, const HttpResponsePtr& c) {
                    REQUIRE(c->getStatusCode() == k201Created);

                    auto search = anonGet("/posts/search");
                    search->setParameter("q", token);
                    client->sendRequest(search,
                        [TEST_CTX](ReqResult, const HttpResponsePtr& resp) {
                            REQUIRE(resp->getStatusCode() == k200OK);
                            auto json = resp->getJsonObject();
                            REQUIRE(json);
                            CHECK((*json)["count"].asUInt() == 1u);
                        });
                });
        });
}

DROGON_TEST(Drafts_AreNotListedOnTheAuthorsPublicProfile)
{
    auto client = HttpClient::newHttpClient(testBaseUrl());
    const std::string token = "draftprofile_" + uniqueSuffix();

    withAuthor(client, "draftprofile",
        [TEST_CTX, client, token](const Author& a) {
            const int uid = a.userId;
            REQUIRE(uid > 0);

            auto create = makePost("Profile draft " + token,
                                   "Confidential body " + token + ".",
                                   /*draft=*/true);
            a.attachAuth(create);

            client->sendRequest(create,
                [TEST_CTX, client, uid, token](ReqResult, const HttpResponsePtr& c) {
                    REQUIRE(c->getStatusCode() == k201Created);

                    client->sendRequest(
                        anonGet("/posts/user/" + std::to_string(uid)),
                        [TEST_CTX, token](ReqResult, const HttpResponsePtr& resp) {
                            REQUIRE(resp->getStatusCode() == k200OK);
                            auto json = resp->getJsonObject();
                            REQUIRE(json);
                            // This endpoint serves whole bodies, so assert on
                            // the content and not merely on the count.
                            for (const auto& p : (*json)["posts"]) {
                                CHECK(p["content"].asString().find(token)
                                          == std::string::npos);
                                CHECK(p["title"].asString().find(token)
                                          == std::string::npos);
                            }
                            CHECK((*json)["posts"].size() == 0u);
                        });
                });
        });
}

DROGON_TEST(Drafts_RejectCommentsLikesAndBookmarksFromOthers)
{
    auto client = HttpClient::newHttpClient(testBaseUrl());
    const std::string token = "draftwrite_" + uniqueSuffix();

    withAuthor(client, "draftowner",
        [TEST_CTX, client, token](const Author& owner) {
            auto create = makePost("Write-target draft " + token,
                                   "Body " + token + ".", /*draft=*/true);
            owner.attachAuth(create);

            client->sendRequest(create,
                [TEST_CTX, client](ReqResult, const HttpResponsePtr& c) {
                    REQUIRE(c->getStatusCode() == k201Created);
                    auto cj = c->getJsonObject();
                    REQUIRE(cj);
                    const int postId = (*cj)["post"]["id"].asInt();
                    REQUIRE(postId > 0);

                    // A *different* signed-in user: these endpoints all
                    // require a session, so the interesting caller is a
                    // stranger who guessed the id, not an anonymous one.
                    withAuthor(client, "draftstranger",
                        [TEST_CTX, client, postId](const Author& other) {
                            const std::string id = std::to_string(postId);

                            Json::Value cbody;
                            cbody["content"] = "planted on someone's draft";
                            auto comment = HttpRequest::newHttpJsonRequest(cbody);
                            comment->setMethod(Post);
                            comment->setPath("/posts/" + id + "/comments");
                            other.attachAuth(comment);

                            auto like = HttpRequest::newHttpRequest();
                            like->setMethod(Post);
                            like->setPath("/posts/" + id + "/like");
                            other.attachAuth(like);

                            auto bookmark = HttpRequest::newHttpRequest();
                            bookmark->setMethod(Post);
                            bookmark->setPath("/posts/" + id + "/bookmark");
                            other.attachAuth(bookmark);

                            client->sendRequest(comment,
                                [TEST_CTX](ReqResult, const HttpResponsePtr& r) {
                                    CHECK(r->getStatusCode() == k404NotFound);
                                });
                            client->sendRequest(like,
                                [TEST_CTX](ReqResult, const HttpResponsePtr& r) {
                                    CHECK(r->getStatusCode() == k404NotFound);
                                });
                            client->sendRequest(bookmark,
                                [TEST_CTX](ReqResult, const HttpResponsePtr& r) {
                                    CHECK(r->getStatusCode() == k404NotFound);
                                });
                        });
                });
        });
}

DROGON_TEST(Drafts_ServeNoCommentThread)
{
    auto client = HttpClient::newHttpClient(testBaseUrl());
    const std::string token = "draftthread_" + uniqueSuffix();

    withAuthor(client, "draftthread",
        [TEST_CTX, client, token](const Author& a) {
            auto create = makePost("Thread draft " + token,
                                   "Body " + token + ".", /*draft=*/true);
            a.attachAuth(create);

            client->sendRequest(create,
                [TEST_CTX, client](ReqResult, const HttpResponsePtr& c) {
                    REQUIRE(c->getStatusCode() == k201Created);
                    auto cj = c->getJsonObject();
                    REQUIRE(cj);
                    const int postId = (*cj)["post"]["id"].asInt();

                    client->sendRequest(
                        anonGet("/posts/" + std::to_string(postId) + "/comments"),
                        [TEST_CTX](ReqResult, const HttpResponsePtr& resp) {
                            REQUIRE(resp->getStatusCode() == k200OK);
                            auto json = resp->getJsonObject();
                            REQUIRE(json);
                            CHECK((*json)["comments"].size() == 0u);
                        });
                });
        });
}
