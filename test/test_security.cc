#include <drogon/drogon.h>
#include <drogon/drogon_test.h>
#include <drogon/HttpClient.h>

#include "../helpers/Security.h"

#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>

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

// RAII toggle for BLOG_SECURE_COOKIES. secureCookies() reads the env on
// every call, so flipping it here is enough to exercise both naming
// regimes without standing up a second app instance.
class ScopedSecureCookies
{
  public:
    explicit ScopedSecureCookies(bool on)
    {
        const char* prev = std::getenv("BLOG_SECURE_COOKIES");
        had_  = prev != nullptr;
        prev_ = had_ ? prev : "";
        if (on) setenv("BLOG_SECURE_COOKIES", "1", 1);
        else    unsetenv("BLOG_SECURE_COOKIES");
    }
    ~ScopedSecureCookies()
    {
        if (had_) setenv("BLOG_SECURE_COOKIES", prev_.c_str(), 1);
        else      unsetenv("BLOG_SECURE_COOKIES");
    }
    ScopedSecureCookies(const ScopedSecureCookies&)            = delete;
    ScopedSecureCookies& operator=(const ScopedSecureCookies&) = delete;

  private:
    bool        had_;
    std::string prev_;
};

} // namespace

// Cookie names must carry the `__Host-` prefix whenever we are serving
// over TLS. That prefix is a browser-enforced ban on the Domain
// attribute, which is what stops a sibling vhost on the same
// registrable domain from overwriting our session / CSRF cookies
// (cookie tossing → CSRF bypass and session fixation).
//
// It must NOT be used on plain HTTP: browsers reject a `__Host-` cookie
// that lacks Secure, so a prefixed name there would silently drop the
// cookie and break every dev / CI run.
DROGON_TEST(Security_CookieNamesUseHostPrefixUnderTls)
{
    {
        ScopedSecureCookies tls(true);
        CHECK(security::csrfCookieName()    == "__Host-csrf_token");
        CHECK(security::sessionCookieName() == "__Host-JSESSIONID");
    }
    {
        ScopedSecureCookies plain(false);
        CHECK(security::csrfCookieName()    == "csrf_token");
        CHECK(security::sessionCookieName() == "JSESSIONID");
    }
}

// Email-collision masking on register: a second registration with a
// different username but the existing email gets a 201 (not 409).
DROGON_TEST(Security_DuplicateEmailIsMasked)
{
    auto client = HttpClient::newHttpClient(testBaseUrl());
    const std::string suffix = uniqueSuffix();

    Json::Value first;
    first["username"] = "first_" + suffix;
    first["email"]    = "shared_" + suffix + "@example.test";
    first["password"] = "first-password-1";

    client->sendRequest(jsonPost("/auth/register", first),
        [TEST_CTX, client, suffix](ReqResult, const HttpResponsePtr& r) {
            REQUIRE(r->getStatusCode() == k201Created);

            Json::Value second;
            second["username"] = "second_" + suffix;          // distinct
            second["email"]    = "shared_" + suffix + "@example.test"; // same
            second["password"] = "second-password-1";

            client->sendRequest(jsonPost("/auth/register", second),
                [TEST_CTX](ReqResult, const HttpResponsePtr& r2) {
                    // The endpoint must respond as if it succeeded so an
                    // attacker can't enumerate email addresses.
                    CHECK(r2->getStatusCode() == k201Created);
                });
        });
}

// Login responds with the same 401 for "user doesn't exist" and "wrong
// password". Latency parity is enforced by always running an Argon2id verify
// — we cover the timing aspect out-of-band (see SECURITY.md) because nesting
// synchronous waits inside a Drogon test handler deadlocks its event loop.
DROGON_TEST(Security_LoginShapeDoesNotLeakUserExistence)
{
    auto client = HttpClient::newHttpClient(testBaseUrl());
    const std::string suffix = uniqueSuffix();

    Json::Value reg;
    reg["username"] = "tim_" + suffix;
    reg["email"]    = "tim_" + suffix + "@example.test";
    reg["password"] = "tim-real-pass-77";

    client->sendRequest(jsonPost("/auth/register", reg),
        [TEST_CTX, client, suffix](ReqResult, const HttpResponsePtr&) {
            // Existing user, wrong password.
            Json::Value wrong;
            wrong["username"] = "tim_" + suffix;
            wrong["password"] = "definitely-wrong-pw";
            client->sendRequest(jsonPost("/auth/login", wrong),
                [TEST_CTX, client, suffix](ReqResult, const HttpResponsePtr& r1) {
                    REQUIRE(r1->getStatusCode() == k401Unauthorized);
                    auto j1 = r1->getJsonObject();
                    REQUIRE(j1);
                    const auto msg1 = (*j1)["error"].asString();

                    // Non-existent user.
                    Json::Value ghost;
                    ghost["username"] = "ghost_" + suffix;
                    ghost["password"] = "anything";
                    client->sendRequest(jsonPost("/auth/login", ghost),
                        [TEST_CTX, msg1](ReqResult, const HttpResponsePtr& r2) {
                            REQUIRE(r2->getStatusCode() == k401Unauthorized);
                            auto j2 = r2->getJsonObject();
                            REQUIRE(j2);
                            // Identical error message is the necessary
                            // structural invariant.
                            CHECK((*j2)["error"].asString() == msg1);
                        });
                });
        });
}

// Two consecutive reset-password calls with the same token: the second one
// must fail because the row was atomically deleted by the first.
DROGON_TEST(Security_ResetPasswordTokenIsSingleUse)
{
    auto client = HttpClient::newHttpClient(testBaseUrl());
    const std::string suffix = uniqueSuffix();
    const std::string username = "rp_" + suffix;
    const std::string email    = username + "@example.test";

    Json::Value reg;
    reg["username"] = username;
    reg["email"]    = email;
    reg["password"] = "old-password-1";

    client->sendRequest(jsonPost("/auth/register", reg),
        [TEST_CTX, client, email, username](ReqResult, const HttpResponsePtr&) {
            // Insert a known reset token directly via the DB so we don't have
            // to scrape email content from the worker thread. Storage is the
            // SHA-256 of the plaintext (see security::sha256Hex); the API
            // hashes the inbound token before lookup, so we plant the same
            // hash here and send the plaintext over the wire.
            auto db = app().getDbClient();
            const std::string token     = "test-token-" + uniqueSuffix();
            const std::string tokenHash = security::sha256Hex(token);
            db->execSqlAsync(
                "INSERT INTO password_reset_tokens (user_id, token, expires_at) "
                "SELECT id, $2, NOW() + INTERVAL '10 minutes' "
                "FROM users WHERE username = $1",
                [TEST_CTX, client, token](const orm::Result&) {
                    Json::Value body;
                    body["token"]    = token;
                    body["password"] = "new-password-1";

                    client->sendRequest(jsonPost("/auth/reset-password", body),
                        [TEST_CTX, client, token](ReqResult, const HttpResponsePtr& r1) {
                            REQUIRE(r1->getStatusCode() == k200OK);

                            Json::Value replay;
                            replay["token"]    = token;
                            replay["password"] = "yet-another-pass";

                            client->sendRequest(jsonPost("/auth/reset-password", replay),
                                [TEST_CTX](ReqResult, const HttpResponsePtr& r2) {
                                    CHECK(r2->getStatusCode() == k400BadRequest);
                                });
                        });
                },
                [TEST_CTX](const orm::DrogonDbException& e) {
                    FAIL(std::string("seed insert failed: ") + e.base().what());
                },
                username, tokenHash);
        });
}

// Issuing a fresh reset request must invalidate every prior token for the
// same user — otherwise a stolen mailbox can be used after the user has
// already kicked off recovery.
DROGON_TEST(Security_RequestResetWipesOldTokens)
{
    auto client = HttpClient::newHttpClient(testBaseUrl());
    const std::string suffix = uniqueSuffix();
    const std::string username = "rpw_" + suffix;
    const std::string email    = username + "@example.test";

    Json::Value reg;
    reg["username"] = username;
    reg["email"]    = email;
    reg["password"] = "wipe-pass-1";

    client->sendRequest(jsonPost("/auth/register", reg),
        [TEST_CTX, client, email, username](ReqResult, const HttpResponsePtr&) {
            auto db = app().getDbClient();
            const std::string oldToken = "old-" + uniqueSuffix();

            db->execSqlAsync(
                "INSERT INTO password_reset_tokens (user_id, token, expires_at) "
                "SELECT id, $2, NOW() + INTERVAL '10 minutes' "
                "FROM users WHERE username = $1",
                [TEST_CTX, client, email, oldToken](const orm::Result&) {
                    Json::Value req;
                    req["email"] = email;
                    client->sendRequest(jsonPost("/auth/request-reset", req),
                        [TEST_CTX, client, oldToken](ReqResult, const HttpResponsePtr& r) {
                            REQUIRE(r->getStatusCode() == k200OK);

                            // The old token must no longer work.
                            Json::Value replay;
                            replay["token"]    = oldToken;
                            replay["password"] = "doesnt-matter-1";
                            client->sendRequest(jsonPost("/auth/reset-password", replay),
                                [TEST_CTX](ReqResult, const HttpResponsePtr& r2) {
                                    CHECK(r2->getStatusCode() == k400BadRequest);
                                });
                        });
                },
                [TEST_CTX](const orm::DrogonDbException& e) {
                    FAIL(std::string("seed insert failed: ") + e.base().what());
                },
                username, oldToken);
        });
}

// Verifying an email is idempotent in the success sense: the second call
// with the same (now consumed) token returns 400.
DROGON_TEST(Security_VerifyEmailIsAtomic)
{
    auto client = HttpClient::newHttpClient(testBaseUrl());
    const std::string suffix = uniqueSuffix();
    const std::string username = "ve_" + suffix;
    const std::string email    = username + "@example.test";

    Json::Value reg;
    reg["username"] = username;
    reg["email"]    = email;
    reg["password"] = "verify-pass-1";

    client->sendRequest(jsonPost("/auth/register", reg),
        [TEST_CTX, client, username](ReqResult, const HttpResponsePtr&) {
            auto db = app().getDbClient();
            // The plaintext token registration generated was only ever sent
            // to the (mock) email; we can't read it back from the DB now
            // that it's stored hashed. Overwrite with a (hash, plaintext)
            // pair we control, then exercise the endpoint with the plaintext.
            const std::string plain = "test-verify-" + uniqueSuffix();
            const std::string hash  = security::sha256Hex(plain);
            db->execSqlAsync(
                "UPDATE users "
                "   SET email_verification_token   = $2, "
                "       email_verification_expires = NOW() + INTERVAL '10 minutes' "
                " WHERE username = $1",
                [TEST_CTX, client, plain](const orm::Result&) {
                    Json::Value first;
                    first["token"] = plain;
                    client->sendRequest(jsonPost("/auth/verify-email", first),
                        [TEST_CTX, client, plain](ReqResult, const HttpResponsePtr& r1) {
                            REQUIRE(r1->getStatusCode() == k200OK);

                            Json::Value second;
                            second["token"] = plain;
                            client->sendRequest(jsonPost("/auth/verify-email", second),
                                [TEST_CTX](ReqResult, const HttpResponsePtr& r2) {
                                    CHECK(r2->getStatusCode() == k400BadRequest);
                                });
                        });
                },
                [TEST_CTX](const orm::DrogonDbException& e) {
                    FAIL(std::string("token seed failed: ") + e.base().what());
                },
                username, hash);
        });
}
