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

// Client-IP resolution is a trust boundary: it keys every per-IP rate
// limit and stamps the access + audit logs. Before this was tightened,
// clientIp() walked a waterfall of CF-Connecting-IP → X-Real-IP → XFF and
// believed whichever turned up first, on the assumption that the reverse
// proxy stripped inbound copies. nginx does not strip anything unless told
// to, so a request that reached the origin directly could name its own
// client IP — a fresh value per request meant a fresh token bucket, i.e.
// no per-IP limit at all, plus attacker-chosen entries in both logs.
//
// The rules asserted here: only a trusted peer may speak for someone else,
// only the one configured header is read, and a chain is reduced to its
// last hop (the entry the nearest trusted proxy actually observed —
// everything before it came from the client).
DROGON_TEST(Security_ClientIpOnlyTrustsProxiesAndOneHeader)
{
    // Untrusted peer: whatever it claims about itself is ignored.
    CHECK(security::resolveClientIp("203.0.113.9", "198.51.100.4")
          == "203.0.113.9");
    CHECK(security::resolveClientIp("203.0.113.9", "") == "203.0.113.9");

    // Trusted peer (loopback is the built-in default) may forward a claim.
    CHECK(security::resolveClientIp("127.0.0.1", "198.51.100.4")
          == "198.51.100.4");
    CHECK(security::resolveClientIp("::1", "198.51.100.4") == "198.51.100.4");

    // No claim, or a blank/whitespace one, falls back to the peer rather
    // than bucketing every such request under the empty-string key.
    CHECK(security::resolveClientIp("127.0.0.1", "")    == "127.0.0.1");
    CHECK(security::resolveClientIp("127.0.0.1", "   ") == "127.0.0.1");

    // A chain collapses to its LAST hop, never the client-controlled head.
    CHECK(security::resolveClientIp("127.0.0.1",
              "198.51.100.4, 203.0.113.9") == "203.0.113.9");
    CHECK(security::resolveClientIp("127.0.0.1",
              "evil, 198.51.100.4,  203.0.113.9  ") == "203.0.113.9");

    // Only one header is consulted, and it is X-Real-IP unless the
    // deployment says otherwise. A stray CF-Connecting-IP is not a
    // second chance to be believed.
    CHECK(security::clientIpHeader() == "X-Real-IP");
}

// Limiter keys contain attacker-controlled IPs/usernames. Cardinality must be
// a hard bound, not merely a stale-entry sweep: a botnet can keep every key
// younger than the TTL and otherwise grow the process until OOM. The final
// key is taken twice to also prove LRU churn leaves normal bucket semantics
// intact.
DROGON_TEST(Security_RateLimitCacheHasAHardCardinalityBound)
{
    const auto before = security::rateLimitStats();
    const std::string namespaceKey = "cardinality_" + uniqueSuffix();

    bool everyFreshKeyWasAllowed = true;
    for (std::size_t i = 0; i < before.capacity + 512; ++i) {
        const auto decision = security::rateLimitTake(
            namespaceKey, "attacker-key-" + std::to_string(i),
            1.0, 1.0 / 3600.0);
        everyFreshKeyWasAllowed = everyFreshKeyWasAllowed && decision.allowed;
    }
    CHECK(everyFreshKeyWasAllowed);

    const auto after = security::rateLimitStats();
    CHECK(after.buckets <= after.capacity);
    CHECK(after.capacity == before.capacity);
    CHECK(after.capacityEvictions > before.capacityEvictions);

    const auto repeated = security::rateLimitTake(
        namespaceKey,
        "attacker-key-" + std::to_string(before.capacity + 511),
        1.0, 1.0 / 3600.0);
    CHECK(!repeated.allowed);

    // A huge request-derived key is represented by a fixed-size digest and
    // remains stable across requests; it cannot become a retained 1 MiB map
    // allocation per entry.
    const std::string hugeKey(1024 * 1024, 'x');
    CHECK(security::rateLimitTake(
              "large_key", hugeKey, 1.0, 1.0 / 3600.0).allowed);
    CHECK(!security::rateLimitTake(
               "large_key", hugeKey, 1.0, 1.0 / 3600.0).allowed);
    CHECK(security::rateLimitStats().buckets <= after.capacity);
}

// Registration never permits credentials beyond these limits, so accepting
// larger login fields can only waste a worker slot / DB parameter / Argon2
// input on attacker data. Both checks must happen synchronously before that
// expensive path.
DROGON_TEST(Security_LoginRejectsImpossibleCredentialSizesEarly)
{
    auto client = HttpClient::newHttpClient(testBaseUrl());

    Json::Value longUsername;
    longUsername["username"] = std::string(33, 'u');
    longUsername["password"] = "valid-length-password";
    client->sendRequest(jsonPost("/auth/login", longUsername),
        [TEST_CTX, client](ReqResult, const HttpResponsePtr& first) {
            REQUIRE(first->getStatusCode() == k400BadRequest);

            Json::Value longPassword;
            longPassword["username"] = "bounded-user";
            longPassword["password"] = std::string(257, 'p');
            client->sendRequest(jsonPost("/auth/login", longPassword),
                [TEST_CTX](ReqResult, const HttpResponsePtr& second) {
                    CHECK(second->getStatusCode() == k400BadRequest);
                });
        });
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

// Policy failure must not consume a legitimate recovery token. Once a valid
// password succeeds, however, the same token is permanently single-use.
DROGON_TEST(Security_ResetPasswordTokenIsConsumedOnlyOnSuccess)
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
                    Json::Value weak;
                    weak["token"]    = token;
                    weak["password"] = "password";

                    client->sendRequest(jsonPost("/auth/reset-password", weak),
                        [TEST_CTX, client, token](ReqResult, const HttpResponsePtr& r0) {
                            REQUIRE(r0->getStatusCode() == k400BadRequest);

                            Json::Value valid;
                            valid["token"]    = token;
                            valid["password"] = "new-password-1";
                            client->sendRequest(jsonPost("/auth/reset-password", valid),
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
