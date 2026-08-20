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
#include <memory>
#include <string>

using namespace drogon;

namespace {

std::string testBaseUrl()
{
    const char* port = std::getenv("TEST_PORT");
    return std::string("http://127.0.0.1:") +
           (port && *port ? port : "18092");
}

std::string uniqueSuffix()
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
    std::string   sessionName;
    std::string   sessionValue;
    std::string   csrfName;
    std::string   csrfValue;

    Client() : http(HttpClient::newHttpClient(testBaseUrl())) {}

    void absorb(const HttpResponsePtr& response)
    {
        for (const auto& cookie : response->getCookies()) {
            const auto& name = cookie.second.key();
            if (name == "JSESSIONID" || name == "__Host-JSESSIONID") {
                sessionName  = name;
                sessionValue = cookie.second.value();
            }
            if (name == "csrf_token" || name == "__Host-csrf_token") {
                csrfName  = name;
                csrfValue = cookie.second.value();
            }
        }
    }

    void apply(const HttpRequestPtr& req) const
    {
        if (!sessionName.empty()) req->addCookie(sessionName, sessionValue);
        if (!csrfName.empty()) {
            req->addCookie(csrfName, csrfValue);
            req->addHeader("X-CSRF-Token", csrfValue);
        }
    }
};

bool isNoStore(const HttpResponsePtr& response)
{
    return response->getHeader("Cache-Control").find("no-store") !=
               std::string::npos &&
           response->getHeader("Pragma") == "no-cache" &&
           response->getHeader("Vary").find("Cookie") != std::string::npos;
}

std::string currentTotpCode(const std::string& secret)
{
    const auto code = totp::generateCode(
        secret, static_cast<std::uint64_t>(std::time(nullptr)));
    char text[7];
    std::snprintf(text, sizeof(text), "%06u", code);
    return text;
}

orm::Result factorState(const std::string& username)
{
    return app().getDbClient()->execSqlSync(
        "SELECT "
        " COALESCE((SELECT enabled FROM user_totp_secrets "
        "   WHERE user_id = u.id), FALSE) AS totp_enabled, "
        " (SELECT count(*) FROM user_totp_secrets "
        "   WHERE user_id = u.id) AS totp, "
        " (SELECT count(*) FROM user_recovery_codes "
        "   WHERE user_id = u.id) AS codes, "
        " (SELECT count(*) FROM user_webauthn_credentials "
        "   WHERE user_id = u.id) AS passkeys, "
        " COALESCE((SELECT string_agg(code_hash, ',' ORDER BY code_hash) "
        "   FROM user_recovery_codes "
        "   WHERE user_id = u.id), '') AS hashes "
        "FROM users u WHERE username = $1",
        username);
}

} // namespace

DROGON_TEST(TwoFactor_TotpEnrollmentRequiresPasswordAndProtectsSecrets)
{
    auto client = std::make_shared<Client>();
    const std::string suffix = uniqueSuffix();
    const std::string user   = "mfa_totp_" + suffix;
    const std::string pass   = "mfa-management-password-1";

    Json::Value registration;
    registration["username"] = user;
    registration["email"]    = user + "@example.test";
    registration["password"] = pass;

    Json::Value login;
    login["username"] = user;
    login["password"] = pass;

    client->http->sendRequest(jsonPost("/auth/register", registration),
        [TEST_CTX, client, login, user, pass]
        (ReqResult, const HttpResponsePtr& registered) {
            REQUIRE(registered->getStatusCode() == k201Created);

            client->http->sendRequest(jsonPost("/auth/login", login),
                [TEST_CTX, client, user, pass]
                (ReqResult, const HttpResponsePtr& loggedIn) {
                    REQUIRE(loggedIn->getStatusCode() == k200OK);
                    client->absorb(loggedIn);
                    REQUIRE(!client->sessionValue.empty());
                    REQUIRE(!client->csrfValue.empty());

                    Json::Value missing(Json::objectValue);
                    auto missingReq = jsonPost("/auth/2fa/totp/setup", missing);
                    client->apply(missingReq);
                    client->http->sendRequest(missingReq,
                        [TEST_CTX, client, user, pass]
                        (ReqResult, const HttpResponsePtr& missingResp) {
                            REQUIRE(missingResp->getStatusCode() == k400BadRequest);

                            Json::Value wrong;
                            wrong["password"] = "not-the-current-password";
                            auto wrongReq = jsonPost("/auth/2fa/totp/setup", wrong);
                            client->apply(wrongReq);
                            client->http->sendRequest(wrongReq,
                                [TEST_CTX, client, user, pass]
                                (ReqResult, const HttpResponsePtr& wrongResp) {
                                    REQUIRE(wrongResp->getStatusCode() == k403Forbidden);

                                    auto db = app().getDbClient();
                                    db->execSqlAsync(
                                        "SELECT count(*) AS n "
                                        "FROM user_totp_secrets s "
                                        "JOIN users u ON u.id = s.user_id "
                                        "WHERE u.username = $1",
                                        [TEST_CTX, client, pass, user]
                                        (const orm::Result& rows) {
                                            REQUIRE(rows[0]["n"].as<int>() == 0);

                                            Json::Value correct;
                                            correct["password"] = pass;
                                            auto setupReq = jsonPost(
                                                "/auth/2fa/totp/setup", correct);
                                            client->apply(setupReq);
                                            client->http->sendRequest(setupReq,
                                                [TEST_CTX, client, user]
                                                (ReqResult,
                                                 const HttpResponsePtr& setupResp) {
                                                    REQUIRE(setupResp->getStatusCode() == k200OK);
                                                    CHECK(isNoStore(setupResp));
                                                    auto setup = setupResp->getJsonObject();
                                                    REQUIRE(setup);
                                                    const auto secret =
                                                        (*setup)["secret"].asString();
                                                    REQUIRE(!secret.empty());

                                                    const auto code = totp::generateCode(
                                                        secret,
                                                        static_cast<std::uint64_t>(
                                                            std::time(nullptr)));
                                                    char codeText[7];
                                                    std::snprintf(codeText,
                                                                  sizeof(codeText),
                                                                  "%06u", code);
                                                    Json::Value confirmation;
                                                    confirmation["code"] = codeText;
                                                    auto forcedFailure = jsonPost(
                                                        "/auth/2fa/totp/confirm",
                                                        confirmation);
                                                    forcedFailure->addHeader(
                                                        "X-Test-Fail-2FA-Transaction",
                                                        "recovery-codes");
                                                    client->apply(forcedFailure);
                                                    client->http->sendRequest(
                                                        forcedFailure,
                                                        [TEST_CTX, client, user,
                                                         confirmation]
                                                        (ReqResult,
                                                         const HttpResponsePtr& failed) {
                                                            REQUIRE(failed->getStatusCode() ==
                                                                    k500InternalServerError);

                                                            const auto state =
                                                                factorState(user);
                                                            REQUIRE(state.size() == 1);
                                                            CHECK(!state[0]["totp_enabled"].as<bool>());
                                                            CHECK(state[0]["codes"].as<int>() == 0);

                                                            auto retry = jsonPost(
                                                                "/auth/2fa/totp/confirm",
                                                                confirmation);
                                                            client->apply(retry);
                                                            client->http->sendRequest(
                                                                retry,
                                                                [TEST_CTX]
                                                                (ReqResult,
                                                                 const HttpResponsePtr& confirmedResp) {
                                                                    REQUIRE(confirmedResp->getStatusCode() == k200OK);
                                                                    CHECK(isNoStore(confirmedResp));
                                                                    auto confirmed = confirmedResp->getJsonObject();
                                                                    REQUIRE(confirmed);
                                                                    CHECK((*confirmed)["enabled"].asBool());
                                                                    CHECK((*confirmed)["recovery_codes"].size() == 10);
                                                                });
                                                        });
                                                });
                                        },
                                        [TEST_CTX](const orm::DrogonDbException& e) {
                                            FAIL(std::string("2FA state query failed: ") +
                                                 e.base().what());
                                        },
                                        user);
                                });
                        });
                });
        });
}

DROGON_TEST(TwoFactor_ManagementMutationsAreAtomic)
{
    auto client = std::make_shared<Client>();
    const std::string suffix = uniqueSuffix();
    const std::string user   = "mfa_txn_" + suffix;
    const std::string pass   = "Atomic-Cedar-9274";
    const std::string secret = totp::generateSecret();

    Json::Value registration;
    registration["username"] = user;
    registration["email"]    = user + "@example.test";
    registration["password"] = pass;

    Json::Value login;
    login["username"] = user;
    login["password"] = pass;

    client->http->sendRequest(jsonPost("/auth/register", registration),
        [TEST_CTX, client, login, user, pass, secret]
        (ReqResult, const HttpResponsePtr& registered) {
            REQUIRE(registered->getStatusCode() == k201Created);
            client->http->sendRequest(jsonPost("/auth/login", login),
                [TEST_CTX, client, user, pass, secret]
                (ReqResult, const HttpResponsePtr& loggedIn) {
                    REQUIRE(loggedIn->getStatusCode() == k200OK);
                    client->absorb(loggedIn);

                    auto db = app().getDbClient();
                    db->execSqlSync(
                        "INSERT INTO user_totp_secrets "
                        "(user_id, secret_b32, enabled, confirmed_at) "
                        "SELECT id, $2, TRUE, NOW() FROM users "
                        "WHERE username = $1",
                        user, security::wrapTotpSecret(secret));
                    db->execSqlSync(
                        "INSERT INTO user_recovery_codes (user_id, code_hash) "
                        "SELECT id, fresh.code_hash FROM users "
                        "CROSS JOIN (VALUES ('old-hash-a'), ('old-hash-b')) "
                        "AS fresh(code_hash) WHERE username = $1",
                        user);
                    db->execSqlSync(
                        "INSERT INTO user_webauthn_credentials "
                        "(user_id, credential_id, public_key, sign_count, nickname) "
                        "SELECT id, $2, decode('a0', 'hex'), 0, 'fixture' "
                        "FROM users WHERE username = $1",
                        user, "fixture-" + user);

                    Json::Value regenerate;
                    regenerate["password"] = pass;
                    auto regenerateReq = jsonPost(
                        "/auth/2fa/recovery-codes/regenerate", regenerate);
                    regenerateReq->addHeader(
                        "X-Test-Fail-2FA-Transaction", "recovery-codes");
                    client->apply(regenerateReq);
                    client->http->sendRequest(regenerateReq,
                        [TEST_CTX, client, user, pass, secret]
                        (ReqResult, const HttpResponsePtr& failedRegenerate) {
                            REQUIRE(failedRegenerate->getStatusCode() ==
                                    k500InternalServerError);

                            const auto afterRegenerate = factorState(user);
                            REQUIRE(afterRegenerate.size() == 1);
                            CHECK(afterRegenerate[0]["totp"].as<int>() == 1);
                            CHECK(afterRegenerate[0]["codes"].as<int>() == 2);
                            CHECK(afterRegenerate[0]["passkeys"].as<int>() == 1);
                            CHECK(afterRegenerate[0]["hashes"].as<std::string>() ==
                                  "old-hash-a,old-hash-b");

                            Json::Value disable;
                            disable["password"]  = pass;
                            disable["totp_code"] = currentTotpCode(secret);
                            auto failedDisable = jsonPost(
                                "/auth/2fa/disable", disable);
                            failedDisable->addHeader(
                                "X-Test-Fail-2FA-Transaction", "disable");
                            client->apply(failedDisable);
                            client->http->sendRequest(
                                failedDisable,
                                [TEST_CTX, client, user, pass, secret]
                                (ReqResult,
                                 const HttpResponsePtr& failedDisableResp) {
                                    REQUIRE(failedDisableResp->getStatusCode() ==
                                            k500InternalServerError);

                                    const auto afterDisable = factorState(user);
                                    REQUIRE(afterDisable.size() == 1);
                                    CHECK(afterDisable[0]["totp"].as<int>() == 1);
                                    CHECK(afterDisable[0]["codes"].as<int>() == 2);
                                    CHECK(afterDisable[0]["passkeys"].as<int>() == 1);

                                    Json::Value disable;
                                    disable["password"]  = pass;
                                    disable["totp_code"] = currentTotpCode(secret);
                                    auto retry = jsonPost(
                                        "/auth/2fa/disable", disable);
                                    client->apply(retry);
                                    client->http->sendRequest(
                                        retry,
                                        [TEST_CTX, user]
                                        (ReqResult,
                                         const HttpResponsePtr& disabled) {
                                            REQUIRE(disabled->getStatusCode() ==
                                                    k200OK);
                                            const auto finalState =
                                                factorState(user);
                                            REQUIRE(finalState.size() == 1);
                                            CHECK(finalState[0]["totp"].as<int>() == 0);
                                            CHECK(finalState[0]["codes"].as<int>() == 0);
                                            CHECK(finalState[0]["passkeys"].as<int>() == 0);
                                        });
                                });
                        });
                });
        });
}

DROGON_TEST(TwoFactor_PasskeyChangesRejectSessionOnlyAuthorization)
{
    auto client = std::make_shared<Client>();
    const std::string suffix = uniqueSuffix();
    const std::string user   = "mfa_key_" + suffix;
    const std::string pass   = "passkey-management-password-1";

    Json::Value registration;
    registration["username"] = user;
    registration["email"]    = user + "@example.test";
    registration["password"] = pass;

    Json::Value login;
    login["username"] = user;
    login["password"] = pass;

    client->http->sendRequest(jsonPost("/auth/register", registration),
        [TEST_CTX, client, login, pass]
        (ReqResult, const HttpResponsePtr& registered) {
            REQUIRE(registered->getStatusCode() == k201Created);
            client->http->sendRequest(jsonPost("/auth/login", login),
                [TEST_CTX, client, pass]
                (ReqResult, const HttpResponsePtr& loggedIn) {
                    REQUIRE(loggedIn->getStatusCode() == k200OK);
                    client->absorb(loggedIn);

                    Json::Value missing(Json::objectValue);
                    auto beginMissing = jsonPost(
                        "/auth/2fa/webauthn/register/begin", missing);
                    client->apply(beginMissing);
                    client->http->sendRequest(beginMissing,
                        [TEST_CTX, client, pass]
                        (ReqResult, const HttpResponsePtr& missingResp) {
                            REQUIRE(missingResp->getStatusCode() == k400BadRequest);

                            Json::Value wrong;
                            wrong["password"] = "wrong-password-for-step-up";
                            auto beginWrong = jsonPost(
                                "/auth/2fa/webauthn/register/begin", wrong);
                            client->apply(beginWrong);
                            client->http->sendRequest(beginWrong,
                                [TEST_CTX, client, pass]
                                (ReqResult, const HttpResponsePtr& wrongResp) {
                                    REQUIRE(wrongResp->getStatusCode() == k403Forbidden);

                                    Json::Value correct;
                                    correct["password"] = pass;
                                    auto beginCorrect = jsonPost(
                                        "/auth/2fa/webauthn/register/begin",
                                        correct);
                                    client->apply(beginCorrect);
                                    client->http->sendRequest(beginCorrect,
                                        [TEST_CTX, client, pass]
                                        (ReqResult,
                                         const HttpResponsePtr& correctResp) {
                                            REQUIRE(correctResp->getStatusCode() == k200OK);
                                            CHECK(isNoStore(correctResp));

                                            Json::Value removeMissingBody(
                                                Json::objectValue);
                                            auto removeMissing = jsonPost(
                                                "/auth/2fa/webauthn/remove/999999999",
                                                removeMissingBody);
                                            client->apply(removeMissing);
                                            client->http->sendRequest(removeMissing,
                                                [TEST_CTX, client, pass]
                                                (ReqResult,
                                                 const HttpResponsePtr& removeMissingResp) {
                                                    REQUIRE(removeMissingResp->getStatusCode() == k400BadRequest);

                                                    Json::Value removeWrongBody;
                                                    removeWrongBody["password"] =
                                                        "wrong-password-for-remove";
                                                    auto removeWrong = jsonPost(
                                                        "/auth/2fa/webauthn/remove/999999999",
                                                        removeWrongBody);
                                                    client->apply(removeWrong);
                                                    client->http->sendRequest(
                                                        removeWrong,
                                                        [TEST_CTX, client, pass]
                                                        (ReqResult,
                                                         const HttpResponsePtr& removeWrongResp) {
                                                            REQUIRE(removeWrongResp->getStatusCode() == k403Forbidden);

                                                            Json::Value removeCorrectBody;
                                                            removeCorrectBody["password"] = pass;
                                                            auto removeCorrect = jsonPost(
                                                                "/auth/2fa/webauthn/remove/999999999",
                                                                removeCorrectBody);
                                                            client->apply(removeCorrect);
                                                            client->http->sendRequest(
                                                                removeCorrect,
                                                                [TEST_CTX]
                                                                (ReqResult,
                                                                 const HttpResponsePtr& removeCorrectResp) {
                                                                    CHECK(removeCorrectResp->getStatusCode() == k404NotFound);
                                                                });
                                                        });
                                                });
                                        });
                                });
                        });
                });
        });
}

DROGON_TEST(TwoFactor_EnrollmentFinishRequiresFreshPasswordAuthorization)
{
    auto client = std::make_shared<Client>();
    const std::string suffix = uniqueSuffix();
    const std::string user   = "mfa_finish_" + suffix;
    const std::string pass   = "finish-authorization-password-1";

    Json::Value registration;
    registration["username"] = user;
    registration["email"]    = user + "@example.test";
    registration["password"] = pass;

    Json::Value login;
    login["username"] = user;
    login["password"] = pass;

    client->http->sendRequest(jsonPost("/auth/register", registration),
        [TEST_CTX, client, login]
        (ReqResult, const HttpResponsePtr& registered) {
            REQUIRE(registered->getStatusCode() == k201Created);
            client->http->sendRequest(jsonPost("/auth/login", login),
                [TEST_CTX, client]
                (ReqResult, const HttpResponsePtr& loggedIn) {
                    REQUIRE(loggedIn->getStatusCode() == k200OK);
                    client->absorb(loggedIn);

                    Json::Value totpBody;
                    totpBody["code"] = "000000";
                    auto totpConfirm = jsonPost(
                        "/auth/2fa/totp/confirm", totpBody);
                    client->apply(totpConfirm);
                    client->http->sendRequest(totpConfirm,
                        [TEST_CTX, client]
                        (ReqResult, const HttpResponsePtr& totpResp) {
                            REQUIRE(totpResp->getStatusCode() == k403Forbidden);

                            Json::Value passkeyBody;
                            passkeyBody["clientDataJSON"] = "not-a-ceremony";
                            passkeyBody["attestationObject"] = "not-a-ceremony";
                            auto passkeyFinish = jsonPost(
                                "/auth/2fa/webauthn/register/finish",
                                passkeyBody);
                            client->apply(passkeyFinish);
                            client->http->sendRequest(passkeyFinish,
                                [TEST_CTX]
                                (ReqResult,
                                 const HttpResponsePtr& passkeyResp) {
                                    CHECK(passkeyResp->getStatusCode() ==
                                          k403Forbidden);
                                });
                        });
                });
        });
}
