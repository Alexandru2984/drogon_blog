#include <drogon/drogon.h>
#include <drogon/drogon_test.h>
#include <drogon/HttpClient.h>

#include "../helpers/PostMeta.h"

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

} // namespace

// ------------------------------------------------------------------ pure

// Slugging is what makes "C++", "c++" and " C ++ " one tag rather than
// three. Everything here is a case that would otherwise fragment a tag.
DROGON_TEST(PostMeta_SlugifyFoldsEquivalentSpellings)
{
    CHECK(post_meta::slugify("C++")        == "c");
    CHECK(post_meta::slugify("c++")        == post_meta::slugify("C++"));
    CHECK(post_meta::slugify("Web Dev")    == "web-dev");
    CHECK(post_meta::slugify("web_dev")    == "web-dev");
    CHECK(post_meta::slugify("  Web  Dev ")== "web-dev");
    CHECK(post_meta::slugify("web---dev")  == "web-dev");
    CHECK(post_meta::slugify("Node.js")    == "node-js");

    // No usable characters at all. The caller must skip these rather than
    // create an empty tag or reject the whole post.
    CHECK(post_meta::slugify("!!!").empty());
    CHECK(post_meta::slugify("   ").empty());
    CHECK(post_meta::slugify("").empty());

    // A leading or trailing separator must not survive into the slug — the
    // schema's CHECK constraint rejects those, so a bug here would surface
    // as a failed insert rather than an ugly slug.
    CHECK(post_meta::slugify("-lead")  == "lead");
    CHECK(post_meta::slugify("trail-") == "trail");
    CHECK(post_meta::slugify(" -x- ")  == "x");

    // Bounded, so the CHECK constraint's 39-character limit cannot be hit
    // by a long tag.
    CHECK(post_meta::slugify(std::string(200, 'a')).size() <= 39);
}

DROGON_TEST(PostMeta_ParseTagsDeduplicatesAndCaps)
{
    Json::Value arr(Json::arrayValue);
    arr.append("C++"); arr.append("c++"); arr.append("CPP");
    auto tags = post_meta::parseTags(arr);
    // "C++" and "c++" fold together; "CPP" is a different slug.
    REQUIRE(tags.size() == 2);
    CHECK(tags[0].slug  == "c");
    CHECK(tags[0].label == "C++");   // first spelling wins for display
    CHECK(tags[1].slug  == "cpp");

    // A comma-separated string is accepted too — a client building the
    // field from a text input will send exactly this.
    auto fromString = post_meta::parseTags(Json::Value("alpha, beta ,gamma"));
    REQUIRE(fromString.size() == 3);
    CHECK(fromString[0].slug == "alpha");
    CHECK(fromString[2].slug == "gamma");

    // Capped, so a payload cannot attach a hundred tags to one post.
    Json::Value many(Json::arrayValue);
    for (int i = 0; i < 50; ++i) many.append("tag" + std::to_string(i));
    CHECK(post_meta::parseTags(many).size() == post_meta::kMaxTagsPerPost);

    // Anything that is not an array or a string yields nothing rather than
    // throwing — the field is optional and a malformed one must not 500.
    CHECK(post_meta::parseTags(Json::Value()).empty());
    CHECK(post_meta::parseTags(Json::Value(42)).empty());
}

DROGON_TEST(PostMeta_ReadingTimeAndExcerpt)
{
    // Never zero: "0 min read" reads as a bug.
    CHECK(post_meta::estimateReadingMinutes("")     == 1);
    CHECK(post_meta::estimateReadingMinutes("word") == 1);

    std::string thousand;
    for (int i = 0; i < 1000; ++i) thousand += "word ";
    CHECK(post_meta::estimateReadingMinutes(thousand) == 5);   // 1000 / 200

    // The excerpt is shown as plain text, so markdown punctuation must not
    // survive into it and fenced code must not appear at all.
    const std::string md =
        "# A Heading\n"
        "Some **bold** text with a [link](https://example.com/very/long).\n"
        "```\n"
        "code that should not appear\n"
        "```\n"
        "- a bullet\n";
    const std::string ex = post_meta::makeExcerpt(md);
    CHECK(ex.find("#")     == std::string::npos);
    CHECK(ex.find("**")    == std::string::npos);
    CHECK(ex.find("https") == std::string::npos);
    CHECK(ex.find("code that should not appear") == std::string::npos);
    CHECK(ex.find("A Heading") != std::string::npos);
    CHECK(ex.find("bold")      != std::string::npos);
    CHECK(ex.find("link")      != std::string::npos);

    // Truncation lands on a word boundary rather than mid-word.
    std::string longText;
    for (int i = 0; i < 200; ++i) longText += "alpha beta ";
    const std::string cut = post_meta::makeExcerpt(longText, 50);
    CHECK(cut.size() <= 60);
    CHECK(cut.back() != 'a');       // would mean a mid-word cut
}

// ------------------------------------------------------------- end to end

// A draft must not be visible to anyone but its author, and must not appear
// in the feed. This is the whole point of the feature: a draft that leaks is
// worse than no draft support at all.
DROGON_TEST(Drafts_AreInvisibleToOthersAndAbsentFromTheFeed)
{
    const std::string a = "drafter_" + uniq();
    const std::string b = "reader_"  + uniq();
    const std::string p = "post-meta-test-password-1";
    auto author = std::make_shared<Client>(testBaseUrl());
    auto other  = std::make_shared<Client>(testBaseUrl());

    Json::Value regA; regA["username"]=a; regA["email"]=a+"@example.test"; regA["password"]=p;
    Json::Value regB; regB["username"]=b; regB["email"]=b+"@example.test"; regB["password"]=p;
    Json::Value logA; logA["username"]=a; logA["password"]=p;
    Json::Value logB; logB["username"]=b; logB["password"]=p;

    Json::Value draft;
    draft["title"]   = "Unfinished";
    draft["content"] = "Half a thought.";
    draft["draft"]   = true;

    author->http->sendRequest(jsonPost("/auth/register", regA),
      [TEST_CTX, author, other, logA, regB, logB, draft](ReqResult, const HttpResponsePtr& r0) {
        REQUIRE(r0->getStatusCode() == k201Created);
        author->http->sendRequest(jsonPost("/auth/login", logA),
          [TEST_CTX, author, other, regB, logB, draft](ReqResult, const HttpResponsePtr& r1) {
            REQUIRE(r1->getStatusCode() == k200OK);
            author->absorb(r1);

            author->http->sendRequest(author->json("/posts", Post, draft),
              [TEST_CTX, author, other, regB, logB](ReqResult, const HttpResponsePtr& r2) {
                REQUIRE(r2->getStatusCode() == k201Created);
                auto j = r2->getJsonObject();
                REQUIRE(j != nullptr);
                CHECK((*j)["post"]["is_draft"].asBool() == true);
                const int id = (*j)["post"]["id"].asInt();
                const std::string path = "/posts/" + std::to_string(id);

                // The author can read it back.
                author->http->sendRequest(author->get(path),
                  [TEST_CTX, author, other, regB, logB, id, path](ReqResult, const HttpResponsePtr& r3) {
                    REQUIRE(r3->getStatusCode() == k200OK);
                    auto jd = r3->getJsonObject();
                    REQUIRE(jd != nullptr);
                    CHECK((*jd)["is_draft"].asBool() == true);

                    // An anonymous reader gets 404 — not 403, which would
                    // confirm the draft exists.
                    auto anon = std::make_shared<Client>(testBaseUrl());
                    anon->http->sendRequest(anon->get(path),
                      [TEST_CTX, author, other, regB, logB, id, path](ReqResult, const HttpResponsePtr& r4) {
                        CHECK(r4->getStatusCode() == k404NotFound);

                        // And so does a different signed-in user.
                        other->http->sendRequest(jsonPost("/auth/register", regB),
                          [TEST_CTX, author, other, logB, id, path](ReqResult, const HttpResponsePtr& r5) {
                            REQUIRE(r5->getStatusCode() == k201Created);
                            other->http->sendRequest(jsonPost("/auth/login", logB),
                              [TEST_CTX, author, other, id, path](ReqResult, const HttpResponsePtr& r6) {
                                REQUIRE(r6->getStatusCode() == k200OK);
                                other->absorb(r6);
                                other->http->sendRequest(other->get(path),
                                  [TEST_CTX, author, id, path](ReqResult, const HttpResponsePtr& r7) {
                                    CHECK(r7->getStatusCode() == k404NotFound);

                                    // Absent from the public feed.
                                    author->http->sendRequest(author->get("/posts?limit=50"),
                                      [TEST_CTX, author, id, path](ReqResult, const HttpResponsePtr& r8) {
                                        REQUIRE(r8->getStatusCode() == k200OK);
                                        auto feed = r8->getJsonObject();
                                        REQUIRE(feed != nullptr);
                                        bool found = false;
                                        for (const auto& post : (*feed)["posts"])
                                            if (post["id"].asInt() == id) found = true;
                                        CHECK(found == false);

                                        // Present in the author's own draft list.
                                        author->http->sendRequest(author->get("/posts/drafts"),
                                          [TEST_CTX, author, id, path](ReqResult, const HttpResponsePtr& r9) {
                                            REQUIRE(r9->getStatusCode() == k200OK);
                                            auto dl = r9->getJsonObject();
                                            REQUIRE(dl != nullptr);
                                            bool inDrafts = false;
                                            for (const auto& post : (*dl)["posts"])
                                                if (post["id"].asInt() == id) inDrafts = true;
                                            CHECK(inDrafts == true);
                                            // Private: never stored by a shared cache.
                                            CHECK(r9->getHeader("Cache-Control")
                                                    .find("no-store") != std::string::npos);

                                            // Publishing makes it visible and
                                            // takes it out of the draft list.
                                            Json::Value pub; pub["draft"] = false;
                                            author->http->sendRequest(
                                              author->json(path, Put, pub),
                                              [TEST_CTX, author, id, path](ReqResult, const HttpResponsePtr& r10) {
                                                REQUIRE(r10->getStatusCode() == k200OK);
                                                auto up = r10->getJsonObject();
                                                REQUIRE(up != nullptr);
                                                CHECK((*up)["post"]["is_draft"].asBool() == false);

                                                auto anon2 = std::make_shared<Client>(testBaseUrl());
                                                anon2->http->sendRequest(anon2->get(path),
                                                  [TEST_CTX](ReqResult, const HttpResponsePtr& r11) {
                                                    CHECK(r11->getStatusCode() == k200OK);
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
      });
}

// Tags round-trip through create, are returned on read, and drive the
// browse-by-tag endpoint. The slug in the URL is normalised the same way as
// on write, so /tags/C++/posts and /tags/c/posts are the same page.
DROGON_TEST(Tags_RoundTripAndBrowse)
{
    const std::string u = "tagger_" + uniq();
    const std::string p = "post-meta-test-password-1";
    const std::string slug = "t" + uniq().substr(0, 12);   // unique per run
    auto c = std::make_shared<Client>(testBaseUrl());

    Json::Value reg; reg["username"]=u; reg["email"]=u+"@example.test"; reg["password"]=p;
    Json::Value log; log["username"]=u; log["password"]=p;

    Json::Value post;
    post["title"]   = "Tagged";
    post["content"] = "Body.";
    post["tags"]    = Json::Value(Json::arrayValue);
    post["tags"].append(slug);
    post["tags"].append(slug);        // duplicate: must collapse

    c->http->sendRequest(jsonPost("/auth/register", reg),
      [TEST_CTX, c, log, post, slug](ReqResult, const HttpResponsePtr& r0) {
        REQUIRE(r0->getStatusCode() == k201Created);
        c->http->sendRequest(jsonPost("/auth/login", log),
          [TEST_CTX, c, post, slug](ReqResult, const HttpResponsePtr& r1) {
            REQUIRE(r1->getStatusCode() == k200OK);
            c->absorb(r1);

            c->http->sendRequest(c->json("/posts", Post, post),
              [TEST_CTX, c, slug](ReqResult, const HttpResponsePtr& r2) {
                REQUIRE(r2->getStatusCode() == k201Created);
                auto j = r2->getJsonObject();
                REQUIRE(j != nullptr);
                REQUIRE((*j)["post"]["tags"].size() == 1);   // deduplicated
                const int id = (*j)["post"]["id"].asInt();

                // Reading the post back returns the tag and the derived
                // metadata.
                c->http->sendRequest(c->get("/posts/" + std::to_string(id)),
                  [TEST_CTX, c, id, slug](ReqResult, const HttpResponsePtr& r3) {
                    REQUIRE(r3->getStatusCode() == k200OK);
                    auto jp = r3->getJsonObject();
                    REQUIRE(jp != nullptr);
                    REQUIRE((*jp)["tags"].size() == 1);
                    CHECK((*jp)["tags"][0]["slug"].asString() == slug);
                    CHECK((*jp)["reading_minutes"].asInt() >= 1);

                    // Browse by tag finds it, with the slug taken through
                    // the same normalisation as on write.
                    c->http->sendRequest(c->get("/tags/" + slug + "/posts"),
                      [TEST_CTX, c, id, slug](ReqResult, const HttpResponsePtr& r4) {
                        REQUIRE(r4->getStatusCode() == k200OK);
                        auto jt = r4->getJsonObject();
                        REQUIRE(jt != nullptr);
                        bool found = false;
                        for (const auto& post : (*jt)["posts"])
                            if (post["id"].asInt() == id) found = true;
                        CHECK(found == true);

                        // Replacing the tag set removes the old association
                        // rather than accumulating.
                        Json::Value edit;
                        edit["tags"] = Json::Value(Json::arrayValue);
                        c->http->sendRequest(
                          c->json("/posts/" + std::to_string(id), Put, edit),
                          [TEST_CTX, c, id, slug](ReqResult, const HttpResponsePtr& r5) {
                            REQUIRE(r5->getStatusCode() == k200OK);
                            CHECK(r5->getJsonObject()->operator[]("post")["tags"].size() == 0);

                            c->http->sendRequest(c->get("/tags/" + slug + "/posts"),
                              [TEST_CTX, id](ReqResult, const HttpResponsePtr& r6) {
                                auto jt2 = r6->getJsonObject();
                                REQUIRE(jt2 != nullptr);
                                for (const auto& post : (*jt2)["posts"])
                                    CHECK(post["id"].asInt() != id);
                              });
                          });
                      });
                  });
              });
          });
      });
}

// A view must count once per viewer per day, and counting it must not
// disturb updated_at. The second part is the subtle one: posts carries a
// BEFORE UPDATE trigger, so without the WHEN guard in migration 0013 every
// read would restamp updated_at — breaking the post's ETag for every other
// reader and turning "last edited" into "last viewed".
DROGON_TEST(Views_CountOncePerViewerAndDoNotTouchUpdatedAt)
{
    const std::string u = "viewer_" + uniq();
    const std::string p = "post-meta-test-password-1";
    auto author = std::make_shared<Client>(testBaseUrl());

    Json::Value reg; reg["username"]=u; reg["email"]=u+"@example.test"; reg["password"]=p;
    Json::Value log; log["username"]=u; log["password"]=p;
    Json::Value post; post["title"]="Counted"; post["content"]="Body.";

    author->http->sendRequest(jsonPost("/auth/register", reg),
      [TEST_CTX, author, log, post](ReqResult, const HttpResponsePtr& r0) {
        REQUIRE(r0->getStatusCode() == k201Created);
        author->http->sendRequest(jsonPost("/auth/login", log),
          [TEST_CTX, author, post](ReqResult, const HttpResponsePtr& r1) {
            REQUIRE(r1->getStatusCode() == k200OK);
            author->absorb(r1);

            author->http->sendRequest(author->json("/posts", Post, post),
              [TEST_CTX, author](ReqResult, const HttpResponsePtr& r2) {
                REQUIRE(r2->getStatusCode() == k201Created);
                const int id = (*r2->getJsonObject())["post"]["id"].asInt();
                const std::string path = "/posts/" + std::to_string(id);

                auto reader = std::make_shared<Client>(testBaseUrl());
                reader->http->sendRequest(reader->get(path),
                  [TEST_CTX, reader, path](ReqResult, const HttpResponsePtr& r3) {
                    REQUIRE(r3->getStatusCode() == k200OK);
                    auto j3 = r3->getJsonObject();
                    REQUIRE(j3 != nullptr);
                    const auto firstCount = (*j3)["view_count"].asInt64();
                    const auto firstUpdated = (*j3)["updated_at"].asString();
                    CHECK(firstCount == 1);

                    // Same reader again the same day: the count holds, and
                    // updated_at is untouched, so the ETag still matches.
                    reader->http->sendRequest(reader->get(path),
                      [TEST_CTX, firstCount, firstUpdated](ReqResult, const HttpResponsePtr& r4) {
                        REQUIRE(r4->getStatusCode() == k200OK);
                        auto j4 = r4->getJsonObject();
                        REQUIRE(j4 != nullptr);
                        CHECK((*j4)["view_count"].asInt64() == firstCount);
                        CHECK((*j4)["updated_at"].asString() == firstUpdated);
                      });
                  });
              });
          });
      });
}
