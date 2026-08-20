#include <drogon/drogon.h>
#include <drogon/drogon_test.h>
#include <drogon/HttpClient.h>

#include "../helpers/Security.h"
#include "../helpers/Totp.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
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

// Carries the session + CSRF cookies forward the way a browser would.
struct Client {
    HttpClientPtr http;
    std::string   sessionCookie;
    std::string   csrf;

    explicit Client(const std::string& base)
        : http(HttpClient::newHttpClient(base)) {}

    void absorb(const HttpResponsePtr& r)
    {
        for (const auto& c : r->getCookies()) {
            if (c.second.key() == "JSESSIONID") sessionCookie = c.second.value();
            if (c.second.key() == "csrf_token") csrf          = c.second.value();
        }
    }

    void apply(const HttpRequestPtr& req) const
    {
        if (!sessionCookie.empty()) req->addCookie("JSESSIONID", sessionCookie);
        if (!csrf.empty()) {
            req->addCookie("csrf_token", csrf);
            req->addHeader("X-CSRF-Token", csrf);
        }
    }
};

} // namespace

// The whole point of the session registry: a user must be able to sign a
// device out and have that take effect. Before this existed, Drogon's
// in-memory store could not be reached from outside the request holding the
// session, so a stolen session was simply valid until the 14-day timeout.
//
// Two browsers, one account. B revokes A's session; A's very next request
// must come back unauthenticated.
DROGON_TEST(Sessions_RevokingOneSessionInvalidatesIt)
{
    const std::string suffix = uniqueSuffix();
    const std::string user   = "sess_" + suffix;
    const std::string pass   = "session-test-password-1";

    auto a = std::make_shared<Client>(testBaseUrl());
    auto b = std::make_shared<Client>(testBaseUrl());

    Json::Value reg;
    reg["username"] = user;
    reg["email"]    = user + "@example.test";
    reg["password"] = pass;

    Json::Value login;
    login["username"] = user;
    login["password"] = pass;

    a->http->sendRequest(jsonPost("/auth/register", reg),
        [TEST_CTX, a, b, login](ReqResult, const HttpResponsePtr& r0) {
            REQUIRE(r0->getStatusCode() == k201Created);

            // Session A signs in.
            a->http->sendRequest(jsonPost("/auth/login", login),
                [TEST_CTX, a, b, login](ReqResult, const HttpResponsePtr& r1) {
                    REQUIRE(r1->getStatusCode() == k200OK);
                    a->absorb(r1);
                    REQUIRE(!a->sessionCookie.empty());

                    // Session B signs in on the same account.
                    b->http->sendRequest(jsonPost("/auth/login", login),
                        [TEST_CTX, a, b](ReqResult, const HttpResponsePtr& r2) {
                            REQUIRE(r2->getStatusCode() == k200OK);
                            b->absorb(r2);

                            // B lists sessions and finds A's alongside its own.
                            auto listReq = HttpRequest::newHttpRequest();
                            listReq->setMethod(Get);
                            listReq->setPath("/auth/sessions");
                            b->apply(listReq);

                            b->http->sendRequest(listReq,
                                [TEST_CTX, a, b](ReqResult, const HttpResponsePtr& r3) {
                                    REQUIRE(r3->getStatusCode() == k200OK);
                                    auto j = r3->getJsonObject();
                                    REQUIRE(j);
                                    const auto& list = (*j)["sessions"];
                                    REQUIRE(list.isArray());
                                    // Both logins are registered, and exactly
                                    // one of them is flagged as the caller's.
                                    CHECK(list.size() >= 2);
                                    int currentCount = 0;
                                    std::string otherSid;
                                    for (const auto& s : list) {
                                        if (s["current"].asBool()) ++currentCount;
                                        else otherSid = s["sid"].asString();
                                    }
                                    CHECK(currentCount == 1);
                                    REQUIRE(!otherSid.empty());

                                    // B revokes the other session.
                                    Json::Value body;
                                    body["sid"] = otherSid;
                                    auto revReq = jsonPost("/auth/sessions/revoke", body);
                                    b->apply(revReq);

                                    b->http->sendRequest(revReq,
                                        [TEST_CTX, a](ReqResult, const HttpResponsePtr& r4) {
                                            REQUIRE(r4->getStatusCode() == k200OK);

                                            // A's next request must be
                                            // unauthenticated. This is the
                                            // assertion the whole feature
                                            // exists for.
                                            auto meReq = HttpRequest::newHttpRequest();
                                            meReq->setMethod(Get);
                                            meReq->setPath("/auth/me");
                                            meReq->addCookie("JSESSIONID", a->sessionCookie);

                                            a->http->sendRequest(meReq,
                                                [TEST_CTX](ReqResult, const HttpResponsePtr& r5) {
                                                    CHECK(r5->getStatusCode() == k401Unauthorized);
                                                });
                                        });
                                });
                        });
                });
        });
}

// A session that is missing from user_sessions cannot be revoked later. A
// registry outage at exactly the wrong moment must therefore fail the login,
// not create an invisible long-lived session. The injected header is compiled
// into blog_test only (BLOG_TEST_BUILD) and exercises Sessions::begin's real
// exception path.
DROGON_TEST(Sessions_RegistryFailureRejectsAuthentication)
{
    const std::string suffix = uniqueSuffix();
    const std::string user   = "registry_fail_" + suffix;
    const std::string pass   = "registry-failure-password-1";
    auto client = std::make_shared<Client>(testBaseUrl());

    Json::Value reg;
    reg["username"] = user;
    reg["email"]    = user + "@example.test";
    reg["password"] = pass;

    Json::Value login;
    login["username"] = user;
    login["password"] = pass;

    client->http->sendRequest(jsonPost("/auth/register", reg),
        [TEST_CTX, client, login, user]
        (ReqResult, const HttpResponsePtr& registered) {
            REQUIRE(registered->getStatusCode() == k201Created);

            auto loginReq = jsonPost("/auth/login", login);
            loginReq->addHeader("X-Test-Fail-Session-Registry", "1");
            client->http->sendRequest(loginReq,
                [TEST_CTX, client, user]
                (ReqResult, const HttpResponsePtr& rejected) {
                    REQUIRE(rejected->getStatusCode() ==
                            k503ServiceUnavailable);
                    CHECK(rejected->getHeader("Retry-After") == "5");
                    client->absorb(rejected);

                    auto db = app().getDbClient();
                    db->execSqlAsync(
                        "SELECT count(*) AS n FROM user_sessions s "
                        "JOIN users u ON u.id = s.user_id "
                        "WHERE u.username = $1 AND s.revoked_at IS NULL",
                        [TEST_CTX, client](const orm::Result& rows) {
                            CHECK(rows[0]["n"].as<int>() == 0);

                            auto me = HttpRequest::newHttpRequest();
                            me->setMethod(Get);
                            me->setPath("/auth/me");
                            if (!client->sessionCookie.empty()) {
                                me->addCookie("JSESSIONID",
                                              client->sessionCookie);
                            }
                            client->http->sendRequest(me,
                                [TEST_CTX]
                                (ReqResult, const HttpResponsePtr& meResp) {
                                    CHECK(meResp->getStatusCode() ==
                                          k401Unauthorized);
                                });
                        },
                        [TEST_CTX](const orm::DrogonDbException& e) {
                            FAIL(std::string("session registry query failed: ") +
                                 e.base().what());
                        },
                        user);
                });
        });
}

DROGON_TEST(Sessions_RegistryFailureRejectsTwoFactorCompletion)
{
    const std::string suffix = uniqueSuffix();
    const std::string user   = "reg2fa_" + suffix;
    const std::string pass   = "Horizon-Copper-9274";
    const std::string secret = totp::generateSecret();
    auto client = std::make_shared<Client>(testBaseUrl());

    Json::Value reg;
    reg["username"] = user;
    reg["email"]    = user + "@example.test";
    reg["password"] = pass;

    Json::Value login;
    login["username"] = user;
    login["password"] = pass;

    client->http->sendRequest(jsonPost("/auth/register", reg),
        [TEST_CTX, client, login, user, secret]
        (ReqResult, const HttpResponsePtr& registered) {
            REQUIRE(registered->getStatusCode() == k201Created);

            auto db = app().getDbClient();
            db->execSqlSync(
                "INSERT INTO user_totp_secrets "
                "(user_id, secret_b32, enabled, confirmed_at) "
                "SELECT id, $2, TRUE, NOW() FROM users WHERE username = $1",
                user, security::wrapTotpSecret(secret));

            client->http->sendRequest(jsonPost("/auth/login", login),
                [TEST_CTX, client, user, secret]
                (ReqResult, const HttpResponsePtr& pending) {
                    REQUIRE(pending->getStatusCode() == k200OK);
                    auto body = pending->getJsonObject();
                    REQUIRE(body);
                    REQUIRE((*body)["requires_2fa"].asBool());
                    client->absorb(pending);
                    REQUIRE(!client->sessionCookie.empty());

                    const auto code = totp::generateCode(
                        secret, static_cast<std::uint64_t>(std::time(nullptr)));
                    char codeText[7];
                    std::snprintf(codeText, sizeof(codeText), "%06u", code);
                    Json::Value verify;
                    verify["code"] = codeText;
                    auto verifyReq = jsonPost("/auth/login/verify-totp", verify);
                    client->apply(verifyReq);
                    verifyReq->addHeader("X-Test-Fail-Session-Registry", "1");

                    client->http->sendRequest(verifyReq,
                        [TEST_CTX, client, user]
                        (ReqResult, const HttpResponsePtr& rejected) {
                            REQUIRE(rejected->getStatusCode() ==
                                    k503ServiceUnavailable);
                            CHECK(rejected->getHeader("Retry-After") == "5");
                            client->absorb(rejected);

                            auto db = app().getDbClient();
                            db->execSqlAsync(
                                "SELECT count(*) AS n FROM user_sessions s "
                                "JOIN users u ON u.id = s.user_id "
                                "WHERE u.username = $1 "
                                "AND s.revoked_at IS NULL",
                                [TEST_CTX, client](const orm::Result& rows) {
                                    CHECK(rows[0]["n"].as<int>() == 0);

                                    auto me = HttpRequest::newHttpRequest();
                                    me->setMethod(Get);
                                    me->setPath("/auth/me");
                                    if (!client->sessionCookie.empty()) {
                                        me->addCookie("JSESSIONID",
                                                      client->sessionCookie);
                                    }
                                    client->http->sendRequest(me,
                                        [TEST_CTX]
                                        (ReqResult,
                                         const HttpResponsePtr& meResp) {
                                            CHECK(meResp->getStatusCode() ==
                                                  k401Unauthorized);
                                        });
                                },
                                [TEST_CTX](const orm::DrogonDbException& e) {
                                    FAIL(std::string(
                                             "session registry query failed: ") +
                                         e.base().what());
                                },
                                user);
                        });
                });
        });
}

// A correct password is not an open-ended partial authentication. The normal
// session lasts 14 days, but the identity waiting for its second factor must
// disappear after ten minutes together with any challenge attached to it.
DROGON_TEST(Sessions_PendingTwoFactorLoginExpires)
{
    const std::string suffix = uniqueSuffix();
    const std::string user   = "expired2fa_" + suffix;
    const std::string pass   = "Juniper-Quartz-7251";
    const std::string secret = totp::generateSecret();
    auto client = std::make_shared<Client>(testBaseUrl());

    Json::Value reg;
    reg["username"] = user;
    reg["email"]    = user + "@example.test";
    reg["password"] = pass;

    Json::Value login;
    login["username"] = user;
    login["password"] = pass;

    client->http->sendRequest(jsonPost("/auth/register", reg),
        [TEST_CTX, client, login, user, secret]
        (ReqResult, const HttpResponsePtr& registered) {
            REQUIRE(registered->getStatusCode() == k201Created);

            app().getDbClient()->execSqlSync(
                "INSERT INTO user_totp_secrets "
                "(user_id, secret_b32, enabled, confirmed_at) "
                "SELECT id, $2, TRUE, NOW() FROM users WHERE username = $1",
                user, security::wrapTotpSecret(secret));

            auto loginReq = jsonPost("/auth/login", login);
            // Test-only fault injection backdates the state at creation; the
            // production binary compiles this header branch out entirely.
            loginReq->addHeader("X-Test-Expire-Pending-2FA", "1");
            client->http->sendRequest(loginReq,
                [TEST_CTX, client, secret]
                (ReqResult, const HttpResponsePtr& pending) {
                    REQUIRE(pending->getStatusCode() == k200OK);
                    auto pendingBody = pending->getJsonObject();
                    REQUIRE(pendingBody);
                    REQUIRE((*pendingBody)["requires_2fa"].asBool());
                    CHECK(pending->getHeader("Cache-Control").find("no-store") !=
                          std::string::npos);
                    client->absorb(pending);
                    REQUIRE(!client->sessionCookie.empty());

                    const auto code = totp::generateCode(
                        secret, static_cast<std::uint64_t>(std::time(nullptr)));
                    char codeText[7];
                    std::snprintf(codeText, sizeof(codeText), "%06u", code);
                    Json::Value verify;
                    verify["code"] = codeText;
                    auto verifyReq = jsonPost("/auth/login/verify-totp", verify);
                    client->apply(verifyReq);

                    client->http->sendRequest(verifyReq,
                        [TEST_CTX, client]
                        (ReqResult, const HttpResponsePtr& expired) {
                            REQUIRE(expired->getStatusCode() == k401Unauthorized);
                            auto body = expired->getJsonObject();
                            REQUIRE(body);
                            CHECK((*body)["error"].asString() ==
                                  "No pending login");

                            auto me = HttpRequest::newHttpRequest();
                            me->setMethod(Get);
                            me->setPath("/auth/me");
                            client->apply(me);
                            client->http->sendRequest(me,
                                [TEST_CTX]
                                (ReqResult, const HttpResponsePtr& meResp) {
                                    CHECK(meResp->getStatusCode() ==
                                          k401Unauthorized);
                                });
                        });
                });
        });
}

// Changing a password must evict other sessions. This is the case users
// most expect to work: changing the password is what someone does when
// they think another party is in their account, and an attacker's session
// surviving it would make the action pointless. The caller's own session is
// kept so they are not signed out of the device they are sitting at.
DROGON_TEST(Sessions_PasswordChangeEvictsOtherSessions)
{
    const std::string suffix = uniqueSuffix();
    const std::string user   = "pw_" + suffix;
    const std::string pass   = "change-me-password-1";
    const std::string newPw  = "changed-me-password-2";

    auto a = std::make_shared<Client>(testBaseUrl());
    auto b = std::make_shared<Client>(testBaseUrl());

    Json::Value reg;
    reg["username"] = user;
    reg["email"]    = user + "@example.test";
    reg["password"] = pass;

    Json::Value login;
    login["username"] = user;
    login["password"] = pass;

    a->http->sendRequest(jsonPost("/auth/register", reg),
        [TEST_CTX, a, b, login, pass, newPw](ReqResult, const HttpResponsePtr& r0) {
            REQUIRE(r0->getStatusCode() == k201Created);

            a->http->sendRequest(jsonPost("/auth/login", login),
                [TEST_CTX, a, b, login, pass, newPw](ReqResult, const HttpResponsePtr& r1) {
                    REQUIRE(r1->getStatusCode() == k200OK);
                    a->absorb(r1);

                    b->http->sendRequest(jsonPost("/auth/login", login),
                        [TEST_CTX, a, b, pass, newPw](ReqResult, const HttpResponsePtr& r2) {
                            REQUIRE(r2->getStatusCode() == k200OK);
                            b->absorb(r2);

                            Json::Value body;
                            body["current_password"] = pass;
                            body["new_password"]     = newPw;
                            auto chReq = jsonPost("/auth/change-password", body);
                            b->apply(chReq);

                            b->http->sendRequest(chReq,
                                [TEST_CTX, a, b](ReqResult, const HttpResponsePtr& r3) {
                                    REQUIRE(r3->getStatusCode() == k200OK);
                                    auto j = r3->getJsonObject();
                                    REQUIRE(j);
                                    CHECK((*j)["revoked_sessions"].asInt() >= 1);

                                    // A is evicted…
                                    auto meA = HttpRequest::newHttpRequest();
                                    meA->setMethod(Get);
                                    meA->setPath("/auth/me");
                                    meA->addCookie("JSESSIONID", a->sessionCookie);
                                    a->http->sendRequest(meA,
                                        [TEST_CTX](ReqResult, const HttpResponsePtr& r4) {
                                            CHECK(r4->getStatusCode() == k401Unauthorized);
                                        });

                                    // …while the session that made the change
                                    // stays signed in.
                                    auto meB = HttpRequest::newHttpRequest();
                                    meB->setMethod(Get);
                                    meB->setPath("/auth/me");
                                    meB->addCookie("JSESSIONID", b->sessionCookie);
                                    b->http->sendRequest(meB,
                                        [TEST_CTX](ReqResult, const HttpResponsePtr& r5) {
                                            CHECK(r5->getStatusCode() == k200OK);
                                        });
                                });
                        });
                });
        });
}

// An emailed password reset is the account-recovery path, so it must be
// stronger than an in-session password change: every browser is signed out,
// including the one that happened to submit the reset. Otherwise an attacker
// who already stole a cookie survives the victim's recovery action.
DROGON_TEST(Sessions_PasswordResetEvictsEverySession)
{
    const std::string suffix = uniqueSuffix();
    const std::string user   = "reset_sess_" + suffix;
    const std::string pass   = "reset-old-password-1";
    const std::string newPw  = "reset-new-password-2";
    const std::string token  = "reset-token-" + suffix;

    auto a = std::make_shared<Client>(testBaseUrl());
    auto b = std::make_shared<Client>(testBaseUrl());

    Json::Value reg;
    reg["username"] = user;
    reg["email"]    = user + "@example.test";
    reg["password"] = pass;

    Json::Value login;
    login["username"] = user;
    login["password"] = pass;

    a->http->sendRequest(jsonPost("/auth/register", reg),
        [TEST_CTX, a, b, login, user, newPw, token](ReqResult, const HttpResponsePtr& r0) {
            REQUIRE(r0->getStatusCode() == k201Created);

            a->http->sendRequest(jsonPost("/auth/login", login),
                [TEST_CTX, a, b, login, user, newPw, token](ReqResult, const HttpResponsePtr& r1) {
                    REQUIRE(r1->getStatusCode() == k200OK);
                    a->absorb(r1);

                    b->http->sendRequest(jsonPost("/auth/login", login),
                        [TEST_CTX, a, b, user, newPw, token](ReqResult, const HttpResponsePtr& r2) {
                            REQUIRE(r2->getStatusCode() == k200OK);
                            b->absorb(r2);

                            auto db = app().getDbClient();
                            db->execSqlAsync(
                                "INSERT INTO password_reset_tokens "
                                "       (user_id, token, expires_at) "
                                "SELECT id, $2, NOW() + INTERVAL '10 minutes' "
                                "  FROM users WHERE username = $1",
                                [TEST_CTX, a, b, user, newPw, token]
                                (const orm::Result&) {
                                    Json::Value reset;
                                    reset["token"]    = token;
                                    reset["password"] = newPw;
                                    a->http->sendRequest(
                                        jsonPost("/auth/reset-password", reset),
                                        [TEST_CTX, a, b, user, newPw]
                                        (ReqResult, const HttpResponsePtr& r3) {
                                            REQUIRE(r3->getStatusCode() == k200OK);
                                            auto body = r3->getJsonObject();
                                            REQUIRE(body);
                                            CHECK((*body)["revoked_sessions"].asInt() >= 2);

                                            auto meA = HttpRequest::newHttpRequest();
                                            meA->setMethod(Get);
                                            meA->setPath("/auth/me");
                                            meA->addCookie("JSESSIONID", a->sessionCookie);
                                            a->http->sendRequest(meA,
                                                [TEST_CTX](ReqResult, const HttpResponsePtr& r4) {
                                                    CHECK(r4->getStatusCode() == k401Unauthorized);
                                                });

                                            auto meB = HttpRequest::newHttpRequest();
                                            meB->setMethod(Get);
                                            meB->setPath("/auth/me");
                                            meB->addCookie("JSESSIONID", b->sessionCookie);
                                            b->http->sendRequest(meB,
                                                [TEST_CTX, b, user, newPw]
                                                (ReqResult, const HttpResponsePtr& r5) {
                                                    CHECK(r5->getStatusCode() == k401Unauthorized);

                                                    Json::Value relogin;
                                                    relogin["username"] = user;
                                                    relogin["password"] = newPw;
                                                    b->http->sendRequest(
                                                        jsonPost("/auth/login", relogin),
                                                        [TEST_CTX](ReqResult, const HttpResponsePtr& r6) {
                                                            CHECK(r6->getStatusCode() == k200OK);
                                                        });
                                                });
                                        });
                                },
                                [TEST_CTX](const orm::DrogonDbException& e) {
                                    FAIL(std::string("reset seed failed: ") +
                                         e.base().what());
                                },
                                user, security::sha256Hex(token));
                        });
                });
        });
}

// The current password is required, and a wrong one must not change
// anything. Otherwise a hijacked session could lock the real owner out of
// their own account without ever knowing the password.
DROGON_TEST(Sessions_PasswordChangeRequiresTheCurrentPassword)
{
    const std::string suffix = uniqueSuffix();
    const std::string user   = "pwbad_" + suffix;
    const std::string pass   = "correct-password-1";

    auto c = std::make_shared<Client>(testBaseUrl());

    Json::Value reg;
    reg["username"] = user;
    reg["email"]    = user + "@example.test";
    reg["password"] = pass;

    Json::Value login;
    login["username"] = user;
    login["password"] = pass;

    c->http->sendRequest(jsonPost("/auth/register", reg),
        [TEST_CTX, c, login](ReqResult, const HttpResponsePtr& r0) {
            REQUIRE(r0->getStatusCode() == k201Created);

            c->http->sendRequest(jsonPost("/auth/login", login),
                [TEST_CTX, c](ReqResult, const HttpResponsePtr& r1) {
                    REQUIRE(r1->getStatusCode() == k200OK);
                    c->absorb(r1);

                    Json::Value body;
                    body["current_password"] = "not-the-password";
                    body["new_password"]     = "some-new-password-1";
                    auto req = jsonPost("/auth/change-password", body);
                    c->apply(req);

                    c->http->sendRequest(req,
                        [TEST_CTX, c](ReqResult, const HttpResponsePtr& r2) {
                            CHECK(r2->getStatusCode() == k403Forbidden);

                            // The session that tried is still usable — a
                            // failed attempt is not a reason to sign anyone
                            // out, and treating it as one would hand any
                            // attacker a denial-of-service.
                            auto me = HttpRequest::newHttpRequest();
                            me->setMethod(Get);
                            me->setPath("/auth/me");
                            me->addCookie("JSESSIONID", c->sessionCookie);
                            c->http->sendRequest(me,
                                [TEST_CTX](ReqResult, const HttpResponsePtr& r3) {
                                    CHECK(r3->getStatusCode() == k200OK);
                                });
                        });
                });
        });
}

// A session id belonging to someone else must not be revocable. The UPDATE
// is scoped by user_id, so a guessed sid matches nothing and the caller
// cannot tell "does not exist" from "not yours".
DROGON_TEST(Sessions_CannotRevokeAnotherUsersSession)
{
    const std::string suffix = uniqueSuffix();
    const std::string victim   = "vic_" + suffix;
    const std::string attacker = "atk_" + suffix;
    const std::string pass     = "shared-shape-password-1";

    auto v = std::make_shared<Client>(testBaseUrl());
    auto a = std::make_shared<Client>(testBaseUrl());

    // By value, not [&]: these lambdas are copied into callbacks that run
    // long after this function has returned, so a reference capture of
    // `pass` would dangle — registration and login would each read whatever
    // was left on the stack, disagree, and the login would 401.
    auto mkReg = [pass](const std::string& u) {
        Json::Value j;
        j["username"] = u;
        j["email"]    = u + "@example.test";
        j["password"] = pass;
        return j;
    };
    auto mkLogin = [pass](const std::string& u) {
        Json::Value j;
        j["username"] = u;
        j["password"] = pass;
        return j;
    };

    v->http->sendRequest(jsonPost("/auth/register", mkReg(victim)),
        [TEST_CTX, v, a, mkReg, mkLogin, victim, attacker](ReqResult, const HttpResponsePtr& r0) {
            REQUIRE(r0->getStatusCode() == k201Created);

            v->http->sendRequest(jsonPost("/auth/login", mkLogin(victim)),
                [TEST_CTX, v, a, mkReg, mkLogin, attacker](ReqResult, const HttpResponsePtr& r1) {
                    REQUIRE(r1->getStatusCode() == k200OK);
                    v->absorb(r1);

                    // Victim reads their own sid.
                    auto listReq = HttpRequest::newHttpRequest();
                    listReq->setMethod(Get);
                    listReq->setPath("/auth/sessions");
                    v->apply(listReq);

                    v->http->sendRequest(listReq,
                        [TEST_CTX, v, a, mkReg, mkLogin, attacker](ReqResult, const HttpResponsePtr& r2) {
                            REQUIRE(r2->getStatusCode() == k200OK);
                            auto j = r2->getJsonObject();
                            REQUIRE(j);
                            REQUIRE((*j)["sessions"].size() >= 1);
                            const std::string victimSid =
                                (*j)["sessions"][0]["sid"].asString();

                            a->http->sendRequest(jsonPost("/auth/register", mkReg(attacker)),
                                [TEST_CTX, v, a, mkLogin, attacker, victimSid](ReqResult, const HttpResponsePtr& r3) {
                                    REQUIRE(r3->getStatusCode() == k201Created);

                                    a->http->sendRequest(jsonPost("/auth/login", mkLogin(attacker)),
                                        [TEST_CTX, v, a, victimSid](ReqResult, const HttpResponsePtr& r4) {
                                            REQUIRE(r4->getStatusCode() == k200OK);
                                            a->absorb(r4);

                                            Json::Value body;
                                            body["sid"] = victimSid;
                                            auto req = jsonPost("/auth/sessions/revoke", body);
                                            a->apply(req);

                                            a->http->sendRequest(req,
                                                [TEST_CTX, v](ReqResult, const HttpResponsePtr& r5) {
                                                    CHECK(r5->getStatusCode() == k404NotFound);

                                                    // Victim is untouched.
                                                    auto me = HttpRequest::newHttpRequest();
                                                    me->setMethod(Get);
                                                    me->setPath("/auth/me");
                                                    me->addCookie("JSESSIONID", v->sessionCookie);
                                                    v->http->sendRequest(me,
                                                        [TEST_CTX](ReqResult, const HttpResponsePtr& r6) {
                                                            CHECK(r6->getStatusCode() == k200OK);
                                                        });
                                                });
                                        });
                                });
                        });
                });
        });
}
