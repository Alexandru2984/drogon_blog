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
    int           id = 0;

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
    HttpRequestPtr verb(const std::string& path, HttpMethod m) const
    {
        auto req = HttpRequest::newHttpRequest();
        req->setMethod(m);
        req->setPath(path);
        apply(req);
        return req;
    }
    HttpRequestPtr get(const std::string& path) const { return verb(path, Get); }
    HttpRequestPtr post(const std::string& path) const { return verb(path, Post); }
    HttpRequestPtr del(const std::string& path) const { return verb(path, Delete); }
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

// register + login + capture the user id, since almost every test here needs
// two real accounts before it can assert anything.
void makeUser(const std::shared_ptr<Client>& c,
              const std::string& name,
              std::function<void()> then)
{
    const std::string pw = "social-test-password-1";
    Json::Value reg; reg["username"]=name; reg["email"]=name+"@example.test"; reg["password"]=pw;
    Json::Value log; log["username"]=name; log["password"]=pw;

    c->http->sendRequest(jsonPost("/auth/register", reg),
      [c, log, then](ReqResult, const HttpResponsePtr&) {
        c->http->sendRequest(jsonPost("/auth/login", log),
          [c, then](ReqResult, const HttpResponsePtr& r) {
            c->absorb(r);
            auto j = r->getJsonObject();
            if (j) c->id = (*j)["user"]["id"].asInt();
            then();
          });
      });
}

} // namespace

// A reply must attach to a comment on the same post. Without the post_id
// check, a reply could be hung off a comment belonging to a different post —
// producing a thread that renders nowhere and a notification pointing at the
// wrong page.
DROGON_TEST(Comments_ReplyMustBelongToTheSamePost)
{
    auto a = std::make_shared<Client>(testBaseUrl());

    makeUser(a, "threader_" + uniq(), [TEST_CTX, a] {
        Json::Value p1; p1["title"]="One"; p1["content"]="First post.";
        Json::Value p2; p2["title"]="Two"; p2["content"]="Second post.";

        a->http->sendRequest(a->json("/posts", Post, p1),
          [TEST_CTX, a, p2](ReqResult, const HttpResponsePtr& r1) {
            REQUIRE(r1->getStatusCode() == k201Created);
            const int postA = (*r1->getJsonObject())["post"]["id"].asInt();

            a->http->sendRequest(a->json("/posts", Post, p2),
              [TEST_CTX, a, postA](ReqResult, const HttpResponsePtr& r2) {
                REQUIRE(r2->getStatusCode() == k201Created);
                const int postB = (*r2->getJsonObject())["post"]["id"].asInt();

                Json::Value c; c["content"] = "Top level on post A.";
                a->http->sendRequest(
                  a->json("/posts/" + std::to_string(postA) + "/comments", Post, c),
                  [TEST_CTX, a, postA, postB](ReqResult, const HttpResponsePtr& r3) {
                    REQUIRE(r3->getStatusCode() == k201Created);
                    auto j3 = r3->getJsonObject();
                    REQUIRE(j3 != nullptr);
                    const int parent = (*j3)["comment"]["id"].asInt();
                    // A top-level comment reports a null parent rather than
                    // omitting the field, so the client never has to guess.
                    CHECK((*j3)["comment"]["parent_id"].isNull());

                    // A reply on the right post works...
                    Json::Value ok; ok["content"]="A reply."; ok["parent_id"]=parent;
                    a->http->sendRequest(
                      a->json("/posts/" + std::to_string(postA) + "/comments", Post, ok),
                      [TEST_CTX, a, postA, postB, parent](ReqResult, const HttpResponsePtr& r4) {
                        REQUIRE(r4->getStatusCode() == k201Created);
                        CHECK((*r4->getJsonObject())["comment"]["parent_id"].asInt() == parent);

                        // ...and the same parent on the other post does not.
                        Json::Value bad; bad["content"]="Wrong thread."; bad["parent_id"]=parent;
                        a->http->sendRequest(
                          a->json("/posts/" + std::to_string(postB) + "/comments", Post, bad),
                          [TEST_CTX, a, postA, parent](ReqResult, const HttpResponsePtr& r5) {
                            CHECK(r5->getStatusCode() == k404NotFound);

                            // The list carries parent_id so the client can
                            // build the tree.
                            a->http->sendRequest(
                              a->get("/posts/" + std::to_string(postA) + "/comments"),
                              [TEST_CTX, parent](ReqResult, const HttpResponsePtr& r6) {
                                REQUIRE(r6->getStatusCode() == k200OK);
                                auto j = r6->getJsonObject();
                                REQUIRE(j != nullptr);
                                REQUIRE((*j)["comments"].size() == 2);
                                CHECK((*j)["comments"][0]["parent_id"].isNull());
                                CHECK((*j)["comments"][1]["parent_id"].asInt() == parent);
                              });
                          });
                      });
                  });
              });
          });
    });
}

// Bookmarking is idempotent in both directions — a double tap must not be an
// error the client has to tell apart from a real failure — and the post
// reports the viewer's own state so the control can render itself.
DROGON_TEST(Bookmarks_AreIdempotentAndReportedPerViewer)
{
    auto a = std::make_shared<Client>(testBaseUrl());

    makeUser(a, "saver_" + uniq(), [TEST_CTX, a] {
        Json::Value p; p["title"]="Saveable"; p["content"]="Body.";
        a->http->sendRequest(a->json("/posts", Post, p),
          [TEST_CTX, a](ReqResult, const HttpResponsePtr& r1) {
            REQUIRE(r1->getStatusCode() == k201Created);
            const int id = (*r1->getJsonObject())["post"]["id"].asInt();
            const std::string bm = "/posts/" + std::to_string(id) + "/bookmark";

            a->http->sendRequest(a->post(bm),
              [TEST_CTX, a, id, bm](ReqResult, const HttpResponsePtr& r2) {
                REQUIRE(r2->getStatusCode() == k200OK);
                CHECK((*r2->getJsonObject())["bookmarked"].asBool() == true);

                // Twice is still bookmarked, not a 409.
                a->http->sendRequest(a->post(bm),
                  [TEST_CTX, a, id, bm](ReqResult, const HttpResponsePtr& r3) {
                    CHECK(r3->getStatusCode() == k200OK);

                    a->http->sendRequest(a->get("/bookmarks"),
                      [TEST_CTX, a, id, bm](ReqResult, const HttpResponsePtr& r4) {
                        REQUIRE(r4->getStatusCode() == k200OK);
                        auto j = r4->getJsonObject();
                        REQUIRE(j != nullptr);
                        // Exactly one entry, not two.
                        int seen = 0;
                        for (const auto& post : (*j)["posts"])
                            if (post["id"].asInt() == id) ++seen;
                        CHECK(seen == 1);
                        // A reading list is nobody else's business.
                        CHECK(r4->getHeader("Cache-Control").find("no-store")
                                != std::string::npos);

                        // The post itself reports the viewer's own state.
                        a->http->sendRequest(a->get("/posts/" + std::to_string(id)),
                          [TEST_CTX, a, id, bm](ReqResult, const HttpResponsePtr& r5) {
                            CHECK((*r5->getJsonObject())["bookmarked"].asBool() == true);

                            // Removing twice is likewise idempotent.
                            a->http->sendRequest(a->del(bm),
                              [TEST_CTX, a, bm](ReqResult, const HttpResponsePtr& r6) {
                                CHECK(r6->getStatusCode() == k200OK);
                                a->http->sendRequest(a->del(bm),
                                  [TEST_CTX](ReqResult, const HttpResponsePtr& r7) {
                                    CHECK(r7->getStatusCode() == k200OK);
                                    CHECK((*r7->getJsonObject())["bookmarked"].asBool() == false);
                                  });
                              });
                          });
                      });
                  });
              });
          });
    });
}

// An anonymous reader must never see someone else's bookmark state, and must
// not be able to reach the reading list at all.
DROGON_TEST(Bookmarks_RequireAuthentication)
{
    auto anon = std::make_shared<Client>(testBaseUrl());
    anon->http->sendRequest(anon->get("/bookmarks"),
      [TEST_CTX](ReqResult, const HttpResponsePtr& r) {
        CHECK(r->getStatusCode() == k401Unauthorized);
      });
    // 403, not 401: the CSRF guard runs ahead of the session check, so a
    // cookie-less POST is rejected as a forgery before anything asks who is
    // making it. That ordering is the right one — it means an unauthenticated
    // write never reaches handler code — so this asserts the actual layering
    // rather than the layering one might assume.
    anon->http->sendRequest(anon->post("/posts/1/bookmark"),
      [TEST_CTX](ReqResult, const HttpResponsePtr& r) {
        CHECK(r->getStatusCode() == k403Forbidden);
      });
}

// Following someone puts their posts in the following feed and notifies
// them once — not once per retry.
DROGON_TEST(Follows_DriveTheFeedAndNotifyOnlyOnTransition)
{
    auto author = std::make_shared<Client>(testBaseUrl());
    auto fan    = std::make_shared<Client>(testBaseUrl());

    makeUser(author, "author_" + uniq(), [TEST_CTX, author, fan] {
        makeUser(fan, "fan_" + uniq(), [TEST_CTX, author, fan] {
            REQUIRE(author->id > 0);
            const std::string f = "/users/" + std::to_string(author->id) + "/follow";

            fan->http->sendRequest(fan->post(f),
              [TEST_CTX, author, fan, f](ReqResult, const HttpResponsePtr& r1) {
                REQUIRE(r1->getStatusCode() == k200OK);
                CHECK((*r1->getJsonObject())["following"].asBool() == true);

                // Re-sending must not produce a second notification.
                fan->http->sendRequest(fan->post(f),
                  [TEST_CTX, author, fan, f](ReqResult, const HttpResponsePtr&) {
                    author->http->sendRequest(author->get("/notifications"),
                      [TEST_CTX, author, fan, f](ReqResult, const HttpResponsePtr& r3) {
                        REQUIRE(r3->getStatusCode() == k200OK);
                        auto j = r3->getJsonObject();
                        REQUIRE(j != nullptr);
                        int follows = 0;
                        for (const auto& n : (*j)["notifications"])
                            if (n["kind"].asString() == "follow") ++follows;
                        CHECK(follows == 1);

                        // A post by the followed author reaches the feed.
                        Json::Value p; p["title"]="For my followers"; p["content"]="Hello.";
                        author->http->sendRequest(author->json("/posts", Post, p),
                          [TEST_CTX, author, fan, f](ReqResult, const HttpResponsePtr& r4) {
                            REQUIRE(r4->getStatusCode() == k201Created);
                            const int pid = (*r4->getJsonObject())["post"]["id"].asInt();

                            fan->http->sendRequest(fan->get("/feed/following"),
                              [TEST_CTX, fan, f, pid](ReqResult, const HttpResponsePtr& r5) {
                                REQUIRE(r5->getStatusCode() == k200OK);
                                bool found = false;
                                for (const auto& post : (*r5->getJsonObject())["posts"])
                                    if (post["id"].asInt() == pid) found = true;
                                CHECK(found == true);

                                // And the follower was told about it.
                                fan->http->sendRequest(fan->get("/notifications"),
                                  [TEST_CTX, fan, f](ReqResult, const HttpResponsePtr& r6) {
                                    bool newPost = false;
                                    for (const auto& n : (*r6->getJsonObject())["notifications"])
                                        if (n["kind"].asString() == "new_post") newPost = true;
                                    CHECK(newPost == true);

                                    // Unfollowing empties the feed again.
                                    fan->http->sendRequest(fan->del(f),
                                      [TEST_CTX, fan](ReqResult, const HttpResponsePtr& r7) {
                                        REQUIRE(r7->getStatusCode() == k200OK);
                                        fan->http->sendRequest(fan->get("/feed/following"),
                                          [TEST_CTX](ReqResult, const HttpResponsePtr& r8) {
                                            CHECK((*r8->getJsonObject())["posts"].size() == 0);
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

// Following yourself would put your own posts in the "people you follow"
// feed. The table's CHECK forbids it; the handler must turn that into a
// clear 400 rather than letting the constraint surface as a 500.
DROGON_TEST(Follows_SelfFollowIsRejected)
{
    auto a = std::make_shared<Client>(testBaseUrl());
    makeUser(a, "narciss_" + uniq(), [TEST_CTX, a] {
        REQUIRE(a->id > 0);
        a->http->sendRequest(a->post("/users/" + std::to_string(a->id) + "/follow"),
          [TEST_CTX](ReqResult, const HttpResponsePtr& r) {
            CHECK(r->getStatusCode() == k400BadRequest);
          });
    });
}

// Notifications go to the person who wants them and to nobody else, and an
// action never notifies its own actor.
DROGON_TEST(Notifications_ReachTheRightPersonAndNeverTheActor)
{
    auto author    = std::make_shared<Client>(testBaseUrl());
    auto commenter = std::make_shared<Client>(testBaseUrl());

    makeUser(author, "notified_" + uniq(), [TEST_CTX, author, commenter] {
        makeUser(commenter, "notifier_" + uniq(), [TEST_CTX, author, commenter] {
            Json::Value p; p["title"]="Discuss"; p["content"]="Thoughts?";
            author->http->sendRequest(author->json("/posts", Post, p),
              [TEST_CTX, author, commenter](ReqResult, const HttpResponsePtr& r1) {
                REQUIRE(r1->getStatusCode() == k201Created);
                const int pid = (*r1->getJsonObject())["post"]["id"].asInt();

                Json::Value c; c["content"] = "Mine.";
                // The author comments on their own post: nobody is notified.
                author->http->sendRequest(
                  author->json("/posts/" + std::to_string(pid) + "/comments", Post, c),
                  [TEST_CTX, author, commenter, pid](ReqResult, const HttpResponsePtr&) {
                    author->http->sendRequest(author->get("/notifications/unread"),
                      [TEST_CTX, author, commenter, pid](ReqResult, const HttpResponsePtr& r3) {
                        REQUIRE(r3->getStatusCode() == k200OK);
                        CHECK((*r3->getJsonObject())["unread"].asInt64() == 0);

                        // Someone else comments: the author hears about it.
                        Json::Value c2; c2["content"] = "Interesting.";
                        commenter->http->sendRequest(
                          commenter->json("/posts/" + std::to_string(pid) + "/comments", Post, c2),
                          [TEST_CTX, author, commenter, pid](ReqResult, const HttpResponsePtr& r4) {
                            REQUIRE(r4->getStatusCode() == k201Created);

                            author->http->sendRequest(author->get("/notifications"),
                              [TEST_CTX, author, commenter](ReqResult, const HttpResponsePtr& r5) {
                                REQUIRE(r5->getStatusCode() == k200OK);
                                auto j = r5->getJsonObject();
                                REQUIRE(j != nullptr);
                                REQUIRE((*j)["notifications"].size() >= 1);
                                CHECK((*j)["unread"].asInt64() >= 1);
                                const auto& n = (*j)["notifications"][0];
                                CHECK(n["kind"].asString() == "comment");
                                CHECK(n["read"].asBool() == false);
                                // Resolved server-side so the client renders
                                // a line without further requests.
                                CHECK(n["actor"]["username"].asString().size() > 0);
                                CHECK(n["post_title"].asString().size() > 0);

                                // The commenter has nothing.
                                commenter->http->sendRequest(commenter->get("/notifications/unread"),
                                  [TEST_CTX, author](ReqResult, const HttpResponsePtr& r6) {
                                    CHECK((*r6->getJsonObject())["unread"].asInt64() == 0);

                                    // Marking all read clears the badge.
                                    author->http->sendRequest(author->post("/notifications/read-all"),
                                      [TEST_CTX, author](ReqResult, const HttpResponsePtr& r7) {
                                        REQUIRE(r7->getStatusCode() == k200OK);
                                        CHECK((*r7->getJsonObject())["marked"].asInt64() >= 1);
                                        author->http->sendRequest(author->get("/notifications/unread"),
                                          [TEST_CTX](ReqResult, const HttpResponsePtr& r8) {
                                            CHECK((*r8->getJsonObject())["unread"].asInt64() == 0);
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

// A reply notifies the person being replied to, and does not also send the
// post's author a duplicate when they are the same person.
DROGON_TEST(Notifications_ReplyNotifiesTheParentAuthorOnce)
{
    auto author  = std::make_shared<Client>(testBaseUrl());
    auto replier = std::make_shared<Client>(testBaseUrl());

    makeUser(author, "parent_" + uniq(), [TEST_CTX, author, replier] {
        makeUser(replier, "child_" + uniq(), [TEST_CTX, author, replier] {
            Json::Value p; p["title"]="Thread"; p["content"]="Start.";
            author->http->sendRequest(author->json("/posts", Post, p),
              [TEST_CTX, author, replier](ReqResult, const HttpResponsePtr& r1) {
                const int pid = (*r1->getJsonObject())["post"]["id"].asInt();
                const std::string cpath = "/posts/" + std::to_string(pid) + "/comments";

                // The post's author comments on their own post, so the
                // parent author and the post author are the same person.
                Json::Value c; c["content"] = "My own comment.";
                author->http->sendRequest(author->json(cpath, Post, c),
                  [TEST_CTX, author, replier, cpath](ReqResult, const HttpResponsePtr& r2) {
                    const int parent = (*r2->getJsonObject())["comment"]["id"].asInt();

                    author->http->sendRequest(author->post("/notifications/read-all"),
                      [TEST_CTX, author, replier, cpath, parent](ReqResult, const HttpResponsePtr&) {
                        Json::Value rep; rep["content"]="Replying."; rep["parent_id"]=parent;
                        replier->http->sendRequest(replier->json(cpath, Post, rep),
                          [TEST_CTX, author](ReqResult, const HttpResponsePtr& r4) {
                            REQUIRE(r4->getStatusCode() == k201Created);
                            author->http->sendRequest(author->get("/notifications"),
                              [TEST_CTX](ReqResult, const HttpResponsePtr& r5) {
                                auto j = r5->getJsonObject();
                                REQUIRE(j != nullptr);
                                int unread = 0;
                                for (const auto& n : (*j)["notifications"])
                                    if (!n["read"].asBool()) ++unread;
                                // Exactly one: a reply to your own comment on
                                // your own post is one event, not two.
                                CHECK(unread == 1);
                              });
                          });
                      });
                  });
              });
        });
    });
}
