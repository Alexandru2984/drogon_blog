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

const char* kPass = "privacy-test-password-1";

} // namespace

// The export is only useful if it is complete. This writes one of everything
// the account can hold, then asserts each turns up — a section that silently
// stops being populated is exactly the failure a person cannot detect from
// the file they receive.
DROGON_TEST(Export_ContainsEverythingTheAccountHolds)
{
    const std::string a = "exp_" + uniq();
    const std::string b = "peer_" + uniq();
    auto me   = std::make_shared<Client>(testBaseUrl());
    auto peer = std::make_shared<Client>(testBaseUrl());

    Json::Value regA; regA["username"]=a; regA["email"]=a+"@example.test"; regA["password"]=kPass;
    Json::Value regB; regB["username"]=b; regB["email"]=b+"@example.test"; regB["password"]=kPass;
    Json::Value logA; logA["username"]=a; logA["password"]=kPass;
    Json::Value logB; logB["username"]=b; logB["password"]=kPass;

    me->http->sendRequest(jsonPost("/auth/register", regA),
      [TEST_CTX, me, peer, logA, regB, logB](ReqResult, const HttpResponsePtr& r0) {
        REQUIRE(r0->getStatusCode() == k201Created);
        me->http->sendRequest(jsonPost("/auth/login", logA),
          [TEST_CTX, me, peer, regB, logB](ReqResult, const HttpResponsePtr& r1) {
            REQUIRE(r1->getStatusCode() == k200OK);
            me->absorb(r1);

            peer->http->sendRequest(jsonPost("/auth/register", regB),
              [TEST_CTX, me, peer, logB](ReqResult, const HttpResponsePtr& r2) {
                REQUIRE(r2->getStatusCode() == k201Created);
                peer->http->sendRequest(jsonPost("/auth/login", logB),
                  [TEST_CTX, me, peer](ReqResult, const HttpResponsePtr& r3) {
                    REQUIRE(r3->getStatusCode() == k200OK);
                    peer->absorb(r3);
                    const int peerId = (*r3->getJsonObject())["user"]["id"].asInt();

                    Json::Value post;
                    post["title"]   = "Exportable";
                    post["content"] = "A post that must appear in the export.";
                    Json::Value tags(Json::arrayValue);
                    tags.append("exporttag" + uniq());
                    post["tags"] = tags;

                    me->http->sendRequest(me->json("/posts", Post, post),
                      [TEST_CTX, me, peer, peerId](ReqResult, const HttpResponsePtr& r4) {
                        REQUIRE(r4->getStatusCode() == k201Created);
                        const int postId = (*r4->getJsonObject())["post"]["id"].asInt();
                        const std::string p = "/posts/" + std::to_string(postId);

                        // Like, save, comment, follow, message: one row in
                        // each of the tables the export claims to cover.
                        Json::Value empty(Json::objectValue);
                        Json::Value comment; comment["content"] = "My own comment.";
                        Json::Value msg;
                        msg["receiver_id"] = peerId;
                        msg["content"]     = "A message that must appear in the export.";

                        me->http->sendRequest(me->json(p + "/like", Post, empty),
                          [TEST_CTX, me, peer, peerId, postId, p, comment, msg, empty](
                              ReqResult, const HttpResponsePtr&) {
                          me->http->sendRequest(me->json(p + "/bookmark", Post, empty),
                            [TEST_CTX, me, peer, peerId, postId, p, comment, msg, empty](
                                ReqResult, const HttpResponsePtr&) {
                            me->http->sendRequest(
                              me->json("/posts/" + std::to_string(postId) + "/comments",
                                       Post, comment),
                              [TEST_CTX, me, peer, peerId, postId, msg, empty](
                                  ReqResult, const HttpResponsePtr&) {
                              me->http->sendRequest(
                                me->json("/users/" + std::to_string(peerId) + "/follow",
                                         Post, empty),
                                [TEST_CTX, me, peerId, postId, msg](
                                    ReqResult, const HttpResponsePtr&) {
                                me->http->sendRequest(me->json("/messages", Post, msg),
                                  [TEST_CTX, me, peerId, postId](
                                      ReqResult, const HttpResponsePtr&) {

                                  Json::Value pw; pw["password"] = kPass;
                                  me->http->sendRequest(
                                    me->json("/account/export", Post, pw),
                                    [TEST_CTX, peerId, postId](
                                        ReqResult, const HttpResponsePtr& rx) {
                                      REQUIRE(rx->getStatusCode() == k200OK);

                                      // Offered as a download, and never
                                      // stored by a shared cache: this body
                                      // is every private message the person
                                      // ever sent.
                                      CHECK(rx->getHeader("Content-Disposition")
                                              .find("attachment") != std::string::npos);
                                      CHECK(rx->getHeader("Cache-Control")
                                              .find("no-store") != std::string::npos);

                                      auto j = rx->getJsonObject();
                                      REQUIRE(j != nullptr);
                                      const auto& e = *j;

                                      CHECK(e["export_version"].asInt() == 1);
                                      CHECK(!e["generated_at"].asString().empty());
                                      CHECK(!e["account"]["username"].asString().empty());
                                      CHECK(!e["account"]["email"].asString().empty());

                                      REQUIRE(e["posts"].size() >= 1u);
                                      bool sawPost = false;
                                      for (const auto& x : e["posts"])
                                          if (x["id"].asInt() == postId) {
                                              sawPost = true;
                                              // Ids are numbers, not quoted
                                              // strings — an export nothing
                                              // can load is not one.
                                              CHECK(x["id"].isIntegral());
                                              CHECK(!x["tags"].asString().empty());
                                          }
                                      CHECK(sawPost == true);

                                      CHECK(e["comments"].size()  >= 1u);
                                      CHECK(e["likes"].size()     >= 1u);
                                      CHECK(e["bookmarks"].size() >= 1u);
                                      REQUIRE(e["following"].size() >= 1u);
                                      CHECK(e["following"][0]["followee_id"].asInt() == peerId);
                                      CHECK(!e["following"][0]["username"].asString().empty());
                                      REQUIRE(e["messages_sent"].size() >= 1u);
                                      CHECK(e["messages_sent"][0]["content"].asString()
                                              .find("must appear") != std::string::npos);
                                      // Present even when empty, so a
                                      // consumer can tell "none" from "this
                                      // export does not cover that".
                                      CHECK(e["messages_received"].isArray());
                                      CHECK(e["notifications"].isArray());
                                      CHECK(e["reports_filed"].isArray());
                                      CHECK(e["sessions"].size() >= 1u);
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
      });
}

// A session cookie says somebody was signed in on this browser once. It does
// not say they are the account holder now, which is the whole question when
// the request is "hand me every private message this person ever sent".
DROGON_TEST(Export_RefusesWithoutTheCorrectPassword)
{
    const std::string u = "expauth_" + uniq();
    auto c = std::make_shared<Client>(testBaseUrl());

    Json::Value reg; reg["username"]=u; reg["email"]=u+"@example.test"; reg["password"]=kPass;
    Json::Value log; log["username"]=u; log["password"]=kPass;

    c->http->sendRequest(jsonPost("/auth/register", reg),
      [TEST_CTX, c, log](ReqResult, const HttpResponsePtr& r0) {
        REQUIRE(r0->getStatusCode() == k201Created);
        c->http->sendRequest(jsonPost("/auth/login", log),
          [TEST_CTX, c](ReqResult, const HttpResponsePtr& r1) {
            REQUIRE(r1->getStatusCode() == k200OK);
            c->absorb(r1);

            Json::Value wrong; wrong["password"] = "not-the-password";
            c->http->sendRequest(c->json("/account/export", Post, wrong),
              [TEST_CTX, c](ReqResult, const HttpResponsePtr& r2) {
                CHECK(r2->getStatusCode() == k403Forbidden);

                Json::Value none(Json::objectValue);
                c->http->sendRequest(c->json("/account/export", Post, none),
                  [TEST_CTX](ReqResult, const HttpResponsePtr& r3) {
                    CHECK(r3->getStatusCode() == k400BadRequest);
                  });
              });
          });
      });

    // No session at all. 403 rather than 401 because the CSRF filter runs
    // ahead of the controller and answers before the session is consulted.
    auto anon = std::make_shared<Client>(testBaseUrl());
    Json::Value pw; pw["password"] = kPass;
    anon->http->sendRequest(jsonPost("/account/export", pw),
      [TEST_CTX](ReqResult, const HttpResponsePtr& r) {
        CHECK((r->getStatusCode() == k403Forbidden ||
               r->getStatusCode() == k401Unauthorized));
      });
}

// Erasure has to actually erase: the content gone, the profile gone, the
// name released, and no way back in. The one thing it must NOT do is take
// other people's replies with it.
DROGON_TEST(Delete_ErasesTheAccountButKeepsOtherPeoplesReplies)
{
    const std::string leaver = "leaver_" + uniq();
    const std::string stayer = "stayer_" + uniq();
    auto go   = std::make_shared<Client>(testBaseUrl());
    auto stay = std::make_shared<Client>(testBaseUrl());

    Json::Value regA; regA["username"]=leaver; regA["email"]=leaver+"@example.test"; regA["password"]=kPass;
    Json::Value regB; regB["username"]=stayer; regB["email"]=stayer+"@example.test"; regB["password"]=kPass;
    Json::Value logA; logA["username"]=leaver; logA["password"]=kPass;
    Json::Value logB; logB["username"]=stayer; logB["password"]=kPass;

    stay->http->sendRequest(jsonPost("/auth/register", regB),
      [TEST_CTX, go, stay, regA, logA, logB, leaver](ReqResult, const HttpResponsePtr& r0) {
        REQUIRE(r0->getStatusCode() == k201Created);
        stay->http->sendRequest(jsonPost("/auth/login", logB),
          [TEST_CTX, go, stay, regA, logA, leaver](ReqResult, const HttpResponsePtr& r1) {
            REQUIRE(r1->getStatusCode() == k200OK);
            stay->absorb(r1);

            // The post belongs to the person who stays, so it survives; the
            // comment on it belongs to the person who leaves.
            Json::Value post;
            post["title"]   = "A thread with a reply";
            post["content"] = "The post itself is not going anywhere.";
            stay->http->sendRequest(stay->json("/posts", Post, post),
              [TEST_CTX, go, stay, regA, logA, leaver](ReqResult, const HttpResponsePtr& r2) {
                REQUIRE(r2->getStatusCode() == k201Created);
                const int postId = (*r2->getJsonObject())["post"]["id"].asInt();
                const std::string cpath = "/posts/" + std::to_string(postId) + "/comments";

                go->http->sendRequest(jsonPost("/auth/register", regA),
                  [TEST_CTX, go, stay, logA, leaver, postId, cpath](
                      ReqResult, const HttpResponsePtr& r3) {
                    REQUIRE(r3->getStatusCode() == k201Created);
                    go->http->sendRequest(jsonPost("/auth/login", logA),
                      [TEST_CTX, go, stay, leaver, postId, cpath](
                          ReqResult, const HttpResponsePtr& r4) {
                        REQUIRE(r4->getStatusCode() == k200OK);
                        go->absorb(r4);
                        const int leaverId = (*r4->getJsonObject())["user"]["id"].asInt();

                        Json::Value c1; c1["content"] = "Question from the leaver.";
                        go->http->sendRequest(go->json(cpath, Post, c1),
                          [TEST_CTX, go, stay, leaver, leaverId, postId, cpath](
                              ReqResult, const HttpResponsePtr& r5) {
                            REQUIRE(r5->getStatusCode() == k201Created);
                            const int parentId =
                                (*r5->getJsonObject())["comment"]["id"].asInt();

                            // …and a reply to it from the person who stays.
                            Json::Value c2;
                            c2["content"]   = "Answer that must survive.";
                            c2["parent_id"] = parentId;
                            stay->http->sendRequest(stay->json(cpath, Post, c2),
                              [TEST_CTX, go, stay, leaver, leaverId, parentId, postId, cpath](
                                  ReqResult, const HttpResponsePtr& r6) {
                                REQUIRE(r6->getStatusCode() == k201Created);
                                const int replyId =
                                    (*r6->getJsonObject())["comment"]["id"].asInt();

                                // Typing the wrong name is refused even with
                                // the right password: the confirmation
                                // exists to stop a mis-click, and a
                                // mis-click that still deletes is not one.
                                Json::Value bad;
                                bad["password"] = kPass;
                                bad["confirm"]  = "something-else";
                                go->http->sendRequest(
                                  go->json("/account/delete", Post, bad),
                                  [TEST_CTX, go, stay, leaver, leaverId, parentId, replyId, postId, cpath](
                                      ReqResult, const HttpResponsePtr& r7) {
                                    CHECK(r7->getStatusCode() == k400BadRequest);

                                    Json::Value ok;
                                    ok["password"] = kPass;
                                    ok["confirm"]  = leaver;
                                    go->http->sendRequest(
                                      go->json("/account/delete", Post, ok),
                                      [TEST_CTX, go, stay, leaver, leaverId, parentId, replyId, postId, cpath](
                                          ReqResult, const HttpResponsePtr& r8) {
                                        REQUIRE(r8->getStatusCode() == k200OK);
                                        auto d = r8->getJsonObject();
                                        REQUIRE(d != nullptr);
                                        CHECK((*d)["deleted"]["comments_tombstoned"].asInt() == 1);
                                        CHECK((*d)["deleted"]["sessions_revoked"].asInt() >= 1);

                                        // The profile is gone. 404, the same
                                        // answer as an id that never
                                        // existed.
                                        auto anon = std::make_shared<Client>(testBaseUrl());
                                        anon->http->sendRequest(
                                          anon->get("/users/" + std::to_string(leaverId)),
                                          [TEST_CTX, go, stay, leaver, parentId, replyId, postId, cpath](
                                              ReqResult, const HttpResponsePtr& r9) {
                                            CHECK(r9->getStatusCode() == k404NotFound);

                                            // The reply survived, and the
                                            // comment it hangs off is a
                                            // tombstone rather than the
                                            // original text.
                                            auto reader = std::make_shared<Client>(testBaseUrl());
                                            reader->http->sendRequest(reader->get(cpath),
                                              [TEST_CTX, leaver, parentId, replyId](
                                                  ReqResult, const HttpResponsePtr& r10) {
                                                REQUIRE(r10->getStatusCode() == k200OK);
                                                auto j = r10->getJsonObject();
                                                REQUIRE(j != nullptr);
                                                bool sawReply = false, sawParent = false;
                                                for (const auto& c : (*j)["comments"]) {
                                                    if (c["id"].asInt() == replyId) {
                                                        sawReply = true;
                                                        CHECK(c["content"].asString()
                                                                .find("must survive")
                                                              != std::string::npos);
                                                    }
                                                    if (c["id"].asInt() == parentId) {
                                                        sawParent = true;
                                                        CHECK(c["content"].asString() == "[deleted]");
                                                    }
                                                }
                                                CHECK(sawReply  == true);
                                                CHECK(sawParent == true);
                                              });

                                            // Signing back in is refused,
                                            // with the same message a wrong
                                            // password gets.
                                            Json::Value again;
                                            again["username"] = leaver;
                                            again["password"] = kPass;
                                            auto fresh = std::make_shared<Client>(testBaseUrl());
                                            fresh->http->sendRequest(
                                              jsonPost("/auth/login", again),
                                              [TEST_CTX](ReqResult, const HttpResponsePtr& r11) {
                                                CHECK(r11->getStatusCode() == k401Unauthorized);
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
      });
}

// The username and email are released by the erasure, so somebody — the same
// person, most likely — can register them again. A tombstone that squatted
// on the name would be a lasting trace of an account that was supposed to be
// gone.
DROGON_TEST(Delete_ReleasesTheUsernameForReuse)
{
    const std::string name = "reuse_" + uniq();
    auto first = std::make_shared<Client>(testBaseUrl());

    Json::Value reg; reg["username"]=name; reg["email"]=name+"@example.test"; reg["password"]=kPass;
    Json::Value log; log["username"]=name; log["password"]=kPass;

    first->http->sendRequest(jsonPost("/auth/register", reg),
      [TEST_CTX, first, log, reg, name](ReqResult, const HttpResponsePtr& r0) {
        REQUIRE(r0->getStatusCode() == k201Created);
        first->http->sendRequest(jsonPost("/auth/login", log),
          [TEST_CTX, first, reg, name](ReqResult, const HttpResponsePtr& r1) {
            REQUIRE(r1->getStatusCode() == k200OK);
            first->absorb(r1);

            Json::Value del; del["password"] = kPass; del["confirm"] = name;
            first->http->sendRequest(first->json("/account/delete", Post, del),
              [TEST_CTX, reg](ReqResult, const HttpResponsePtr& r2) {
                REQUIRE(r2->getStatusCode() == k200OK);

                auto second = std::make_shared<Client>(testBaseUrl());
                second->http->sendRequest(jsonPost("/auth/register", reg),
                  [TEST_CTX](ReqResult, const HttpResponsePtr& r3) {
                    CHECK(r3->getStatusCode() == k201Created);
                  });
              });
          });
      });
}
