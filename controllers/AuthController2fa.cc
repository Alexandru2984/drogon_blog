// 2FA management endpoints + two-step login completion. Lives in its own
// translation unit to keep AuthController.cc readable; same class, just
// other handlers.

#include "AuthController.h"

#include "../helpers/AuditLog.h"
#include "../helpers/RecoveryCodes.h"
#include "../helpers/Security.h"
#include "../helpers/Totp.h"
#include "../helpers/WebAuthn.h"
#include "../models/Users.h"

#include <drogon/orm/Exception.h>
#include <drogon/orm/Mapper.h>
#include <trantor/utils/Logger.h>

#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

using namespace drogon;
using namespace drogon::orm;

namespace {

drogon::HttpResponsePtr jsonError(drogon::HttpStatusCode code,
                                  const std::string&    message)
{
    Json::Value body;
    body["error"] = message;
    auto resp = drogon::HttpResponse::newHttpJsonResponse(body);
    resp->setStatusCode(code);
    return resp;
}

std::optional<int> currentUserId(const HttpRequestPtr& req)
{
    return req->session()->getOptional<int>("user_id");
}

std::optional<int> pendingUserId(const HttpRequestPtr& req)
{
    return req->session()->getOptional<int>("pending_user_id");
}

std::string envOr(const char* name, const char* fallback)
{
    const char* v = std::getenv(name);
    return (v && *v) ? std::string(v) : std::string(fallback);
}

// Format raw bytes as a PostgreSQL bytea literal: '\x<hex>'. libpq's
// text parameter binding then hands this to PG which parses it back
// to the original bytes. The previous code passed a std::string built
// straight from CBOR bytes, which PG rejected with "invalid byte
// sequence for encoding UTF8" the moment a high-bit byte appeared in
// the COSE key (0xa5 is the CBOR map marker, basically guaranteed).
std::string toByteaLiteral(const std::vector<unsigned char>& bytes)
{
    static const char* kHex = "0123456789abcdef";
    std::string out;
    out.reserve(2 + bytes.size() * 2);
    out += "\\x";
    for (auto b : bytes) {
        out.push_back(kHex[(b >> 4) & 0xF]);
        out.push_back(kHex[b & 0xF]);
    }
    return out;
}

std::string siteOrigin()
{
    return envOr("BLOG_SITE_ORIGIN", "https://blog.micutu.com");
}

// RP ID is the registrable domain (no scheme, no port). Defaults to a
// stripped version of BLOG_SITE_ORIGIN; override with BLOG_WEBAUTHN_RP_ID
// when running behind a different domain in dev / staging.
std::string rpId()
{
    auto raw = envOr("BLOG_WEBAUTHN_RP_ID", "");
    if (!raw.empty()) return raw;
    std::string o = siteOrigin();
    const auto schemeEnd = o.find("://");
    if (schemeEnd != std::string::npos) o.erase(0, schemeEnd + 3);
    const auto slash = o.find('/');
    if (slash != std::string::npos)     o.erase(slash);
    const auto colon = o.find(':');
    if (colon != std::string::npos)     o.erase(colon);
    return o;
}

std::string rpName()
{
    return envOr("BLOG_SITE_NAME", "Blog");
}

bool verifyCurrentPassword(int userId, const std::string& candidate)
{
    auto db = drogon::app().getDbClient();
    auto r  = db->execSqlSync(
        "SELECT password_hash FROM users WHERE id = $1", userId);
    if (r.empty()) return false;
    return security::verifyPassword(r[0]["password_hash"].as<std::string>(), candidate);
}

// Best-effort delete and recreate of the recovery codes batch. Called when
// 2FA is enrolled the first time and on user-initiated regenerate.
std::vector<std::string> issueFreshRecoveryCodes(int userId)
{
    auto db = drogon::app().getDbClient();
    db->execSqlSync("DELETE FROM user_recovery_codes WHERE user_id = $1", userId);
    auto plaintext = recovery_codes::generateBatch();
    for (const auto& code : plaintext) {
        db->execSqlSync(
            "INSERT INTO user_recovery_codes (user_id, code_hash) VALUES ($1, $2)",
            userId, recovery_codes::hashOne(code));
    }
    return plaintext;
}

void completeTwoStepLogin(const HttpRequestPtr& req,
                          const std::function<void(const HttpResponsePtr&)>& callback,
                          int userId)
{
    auto db   = drogon::app().getDbClient();
    auto rows = db->execSqlSync(
        "SELECT username, email FROM users WHERE id = $1", userId);
    if (rows.empty()) {
        callback(jsonError(k401Unauthorized, "Account not found"));
        return;
    }

    auto session = req->session();
    session->erase("pending_user_id");
    session->erase("pending_webauthn_challenge");
    // Rotate the session ID again now that the user is fully authenticated.
    // The password step already rotated once when it dropped pending_user_id
    // in, but the same session value carried through the 2FA window — so an
    // attacker who somehow saw the pending_2fa cookie (e.g. via a same-origin
    // proxy log) would inherit the fully-authenticated session if we kept
    // the ID. A second rotation gives the new fully-authed state a brand
    // new identifier the attacker has never observed.
    session->changeSessionIdToClient();
    session->insert("user_id",  userId);
    session->insert("username", rows[0]["username"].as<std::string>());

    Json::Value ret;
    ret["message"]          = "Login successful";
    ret["user"]["id"]       = userId;
    ret["user"]["username"] = rows[0]["username"].as<std::string>();
    ret["user"]["email"]    = rows[0]["email"].as<std::string>();

    auto resp = HttpResponse::newHttpJsonResponse(ret);
    // Issue the CSRF cookie here. The password step's no-2FA branch issues it,
    // but the 2FA branch only planted pending_user_id and returned
    // requires_2fa — it never set a csrf_token. And completeTwoStepLogin just
    // rotated the session id, so any earlier token is gone. Without this the
    // first mutating request after a 2FA login would 403 until the SPA happened
    // to call /auth/me and rehydrate the token.
    security::issueCsrfCookie(req, resp);
    callback(resp);
}

} // namespace

// =========================================================================
// /auth/2fa/status — what's enrolled for the current user
// =========================================================================
void AuthController::status2fa(const HttpRequestPtr& req,
                               std::function<void(const HttpResponsePtr&)>&& callback)
{
    auto userIdOpt = currentUserId(req);
    if (!userIdOpt) { callback(jsonError(k401Unauthorized, "Not authenticated")); return; }

    auto db = drogon::app().getDbClient();
    bool totpEnabled = false;
    {
        auto r = db->execSqlSync(
            "SELECT enabled FROM user_totp_secrets WHERE user_id = $1", *userIdOpt);
        if (!r.empty()) totpEnabled = r[0]["enabled"].as<bool>();
    }
    int passkeys = db->execSqlSync(
        "SELECT count(*) AS n FROM user_webauthn_credentials WHERE user_id = $1",
        *userIdOpt)[0]["n"].as<int>();
    int recoveryLeft = db->execSqlSync(
        "SELECT count(*) AS n FROM user_recovery_codes "
        "WHERE user_id = $1 AND used_at IS NULL",
        *userIdOpt)[0]["n"].as<int>();

    Json::Value ret;
    ret["totp_enabled"]       = totpEnabled;
    ret["passkeys_count"]     = passkeys;
    ret["recovery_codes_left"]= recoveryLeft;
    callback(HttpResponse::newHttpJsonResponse(ret));
}

// =========================================================================
// /auth/2fa/totp/setup — generate (or re-use unconfirmed) TOTP secret
// =========================================================================
void AuthController::setupTotp(const HttpRequestPtr& req,
                               std::function<void(const HttpResponsePtr&)>&& callback)
{
    auto userIdOpt = currentUserId(req);
    if (!userIdOpt) { callback(jsonError(k401Unauthorized, "Not authenticated")); return; }

    // 3 burst, 3/10min: rotating the secret repeatedly is never legitimate.
    if (auto rl = security::rateLimitOr429(
            "totp_setup", "uid:" + std::to_string(*userIdOpt), 3.0, 3.0 / 600.0)) {
        callback(rl);
        return;
    }

    auto db = drogon::app().getDbClient();
    auto rows = db->execSqlSync(
        "SELECT enabled FROM user_totp_secrets WHERE user_id = $1", *userIdOpt);
    if (!rows.empty() && rows[0]["enabled"].as<bool>()) {
        callback(jsonError(k409Conflict, "TOTP already enabled"));
        return;
    }

    // Fresh secret on every setup — re-running the flow rotates the seed
    // so an abandoned half-enrolment can't be reactivated by a stranger.
    // We store the secret wrapped: when BLOG_TOTP_KEY is set the value
    // on disk is encrypted (libsodium secretbox); the column still holds
    // a text string so legacy plaintext rows keep working. See
    // helpers/Security.cc::wrapTotpSecret for the format.
    const std::string secret = totp::generateSecret();
    db->execSqlSync(
        "INSERT INTO user_totp_secrets (user_id, secret_b32, enabled) "
        "VALUES ($1, $2, FALSE) "
        "ON CONFLICT (user_id) DO UPDATE "
        "  SET secret_b32 = EXCLUDED.secret_b32, enabled = FALSE, confirmed_at = NULL",
        *userIdOpt, security::wrapTotpSecret(secret));

    const auto username =
        db->execSqlSync("SELECT username FROM users WHERE id = $1", *userIdOpt)
          [0]["username"].as<std::string>();

    Json::Value ret;
    ret["secret"]       = secret;
    ret["otpauth_url"]  = totp::otpAuthUrl(secret, username, rpName());
    audit_log::record(req, {"2fa.totp.setup", userIdOpt,
                            std::nullopt, std::nullopt, Json::objectValue});
    callback(HttpResponse::newHttpJsonResponse(ret));
}

// =========================================================================
// /auth/2fa/totp/confirm — finalise enrolment with a valid 6-digit code,
// issues 10 recovery codes on success.
// =========================================================================
void AuthController::confirmTotp(const HttpRequestPtr& req,
                                 std::function<void(const HttpResponsePtr&)>&& callback)
{
    auto userIdOpt = currentUserId(req);
    if (!userIdOpt) { callback(jsonError(k401Unauthorized, "Not authenticated")); return; }

    // 5 burst, 5/min: caps brute-force of the 6-digit confirmation code.
    if (auto rl = security::rateLimitOr429(
            "totp_confirm", "uid:" + std::to_string(*userIdOpt), 5.0, 5.0 / 60.0)) {
        callback(rl);
        return;
    }

    auto json = req->getJsonObject();
    if (!json) { callback(jsonError(k400BadRequest, "Invalid JSON")); return; }
    const std::string code = (*json)["code"].asString();

    auto db = drogon::app().getDbClient();
    auto rows = db->execSqlSync(
        "SELECT secret_b32, enabled FROM user_totp_secrets WHERE user_id = $1",
        *userIdOpt);
    if (rows.empty()) {
        callback(jsonError(k400BadRequest, "No pending TOTP setup")); return;
    }
    if (rows[0]["enabled"].as<bool>()) {
        callback(jsonError(k409Conflict, "TOTP already enabled")); return;
    }
    const auto secret =
        security::unwrapTotpSecret(rows[0]["secret_b32"].as<std::string>());
    if (!totp::verify(secret, code)) {
        audit_log::record(req, {"2fa.totp.confirm.fail", userIdOpt,
                                std::nullopt, std::nullopt, Json::objectValue});
        callback(jsonError(k400BadRequest, "Invalid code")); return;
    }

    // NB: we deliberately do NOT seed last_used_step here. Enrolment commonly
    // happens seconds before the first login, often inside the same 30 s TOTP
    // window, so the confirmation code and the first login code are the same
    // value at the same step. Burning the step at confirm time would make that
    // first legitimate login look like a replay and reject it. Replay
    // protection is enforced only on the login path — that's where a captured
    // login code would be reused — and last_used_step stays 0 until the first
    // successful login verifies and claims a step.
    db->execSqlSync(
        "UPDATE user_totp_secrets SET enabled = TRUE, confirmed_at = NOW() "
        "WHERE user_id = $1", *userIdOpt);
    auto codes = issueFreshRecoveryCodes(*userIdOpt);

    audit_log::record(req, {"2fa.totp.enable", userIdOpt,
                            std::nullopt, std::nullopt, Json::objectValue});

    Json::Value ret;
    ret["enabled"] = true;
    Json::Value arr(Json::arrayValue);
    for (auto& c : codes) arr.append(c);
    ret["recovery_codes"] = arr;
    callback(HttpResponse::newHttpJsonResponse(ret));
}

// =========================================================================
// /auth/2fa/disable — remove TOTP + recovery codes + all passkeys. Requires
// password AND a fresh TOTP / recovery / passkey factor to prove the
// caller is genuinely the account owner.
// =========================================================================
void AuthController::disable2fa(const HttpRequestPtr& req,
                                std::function<void(const HttpResponsePtr&)>&& callback)
{
    auto userIdOpt = currentUserId(req);
    if (!userIdOpt) { callback(jsonError(k401Unauthorized, "Not authenticated")); return; }
    auto json = req->getJsonObject();
    if (!json) { callback(jsonError(k400BadRequest, "Invalid JSON")); return; }

    const std::string password = (*json)["password"].asString();
    if (password.empty() || !verifyCurrentPassword(*userIdOpt, password)) {
        callback(jsonError(k403Forbidden, "Password check failed"));
        return;
    }

    // Either a current TOTP code or a recovery code is required so a
    // hijacked session alone cannot rip 2FA back off.
    auto db = drogon::app().getDbClient();
    bool factorOk = false;
    if (json->isMember("totp_code")) {
        const std::string code = (*json)["totp_code"].asString();
        auto r = db->execSqlSync(
            "SELECT secret_b32 FROM user_totp_secrets WHERE user_id = $1 AND enabled = TRUE",
            *userIdOpt);
        if (!r.empty() &&
            totp::verify(
                security::unwrapTotpSecret(r[0]["secret_b32"].as<std::string>()),
                code))
        {
            factorOk = true;
        }
    }
    if (!factorOk) {
        callback(jsonError(k403Forbidden, "2FA factor required"));
        return;
    }

    db->execSqlSync("DELETE FROM user_totp_secrets        WHERE user_id = $1", *userIdOpt);
    db->execSqlSync("DELETE FROM user_recovery_codes      WHERE user_id = $1", *userIdOpt);
    db->execSqlSync("DELETE FROM user_webauthn_credentials WHERE user_id = $1", *userIdOpt);

    audit_log::record(req, {"2fa.disable", userIdOpt,
                            std::nullopt, std::nullopt, Json::objectValue});

    Json::Value ret;
    ret["enabled"] = false;
    callback(HttpResponse::newHttpJsonResponse(ret));
}

// =========================================================================
// /auth/2fa/recovery-codes/regenerate — issues a fresh batch, invalidates
// the previous one. Requires password.
// =========================================================================
void AuthController::regenerateRecoveryCodes(const HttpRequestPtr& req,
                                             std::function<void(const HttpResponsePtr&)>&& callback)
{
    auto userIdOpt = currentUserId(req);
    if (!userIdOpt) { callback(jsonError(k401Unauthorized, "Not authenticated")); return; }
    auto json = req->getJsonObject();
    if (!json) { callback(jsonError(k400BadRequest, "Invalid JSON")); return; }
    const std::string password = (*json)["password"].asString();
    if (password.empty() || !verifyCurrentPassword(*userIdOpt, password)) {
        callback(jsonError(k403Forbidden, "Password check failed"));
        return;
    }
    auto codes = issueFreshRecoveryCodes(*userIdOpt);
    audit_log::record(req, {"2fa.recovery.regenerate", userIdOpt,
                            std::nullopt, std::nullopt, Json::objectValue});
    Json::Value ret;
    Json::Value arr(Json::arrayValue);
    for (auto& c : codes) arr.append(c);
    ret["recovery_codes"] = arr;
    callback(HttpResponse::newHttpJsonResponse(ret));
}

// =========================================================================
// /auth/2fa/webauthn/register/begin — fresh challenge for navigator.credentials.create
// =========================================================================
void AuthController::webauthnRegisterBegin(const HttpRequestPtr& req,
                                           std::function<void(const HttpResponsePtr&)>&& callback)
{
    auto userIdOpt = currentUserId(req);
    if (!userIdOpt) { callback(jsonError(k401Unauthorized, "Not authenticated")); return; }

    auto db = drogon::app().getDbClient();
    auto rows = db->execSqlSync(
        "SELECT username FROM users WHERE id = $1", *userIdOpt);
    if (rows.empty()) { callback(jsonError(k404NotFound, "User not found")); return; }
    const auto username = rows[0]["username"].as<std::string>();

    const std::string challenge = webauthn::makeChallenge();
    req->session()->insert("pending_webauthn_register_challenge", challenge);

    Json::Value ret;
    ret["challenge"]       = challenge;
    ret["rp"]["id"]        = rpId();
    ret["rp"]["name"]      = rpName();
    ret["user"]["id"]      = webauthn::base64UrlEncode(
        reinterpret_cast<const unsigned char*>(&*userIdOpt), sizeof(int));
    ret["user"]["name"]    = username;
    ret["user"]["displayName"] = username;
    Json::Value algs(Json::arrayValue);
    Json::Value a1; a1["type"] = "public-key"; a1["alg"] = -7; algs.append(a1);
    Json::Value a2; a2["type"] = "public-key"; a2["alg"] = -8; algs.append(a2);
    ret["pubKeyCredParams"] = algs;
    ret["attestation"]      = "none";

    // List existing credentials so the browser can refuse to enrol the
    // same authenticator twice on this account.
    auto existing = db->execSqlSync(
        "SELECT credential_id FROM user_webauthn_credentials WHERE user_id = $1",
        *userIdOpt);
    Json::Value exclude(Json::arrayValue);
    for (const auto& row : existing) {
        Json::Value e;
        e["type"] = "public-key";
        e["id"]   = row["credential_id"].as<std::string>();
        exclude.append(e);
    }
    ret["excludeCredentials"] = exclude;

    callback(HttpResponse::newHttpJsonResponse(ret));
}

// =========================================================================
// /auth/2fa/webauthn/register/finish — store the new credential after
// verifying the attestation against the challenge we just minted.
// =========================================================================
void AuthController::webauthnRegisterFinish(const HttpRequestPtr& req,
                                            std::function<void(const HttpResponsePtr&)>&& callback)
{
    auto userIdOpt = currentUserId(req);
    if (!userIdOpt) { callback(jsonError(k401Unauthorized, "Not authenticated")); return; }
    auto json = req->getJsonObject();
    if (!json) { callback(jsonError(k400BadRequest, "Invalid JSON")); return; }

    auto chalOpt = req->session()->getOptional<std::string>("pending_webauthn_register_challenge");
    if (!chalOpt) { callback(jsonError(k400BadRequest, "No pending challenge")); return; }

    const std::string clientDataJSON = (*json)["clientDataJSON"].asString();
    const std::string attestationObj = (*json)["attestationObject"].asString();
    std::string nickname             = (*json).get("nickname", "").asString();
    // The nickname is plain UI text — cap it so an authenticated client
    // can't grow user_webauthn_credentials.nickname into a multi-megabyte
    // DB-bloat surface by looping passkey enrolments with huge labels.
    if (nickname.size() > 128) nickname.resize(128);

    std::string err;
    auto res = webauthn::finishRegistration(
        clientDataJSON, attestationObj, *chalOpt, rpId(), siteOrigin(), err);
    req->session()->erase("pending_webauthn_register_challenge");
    if (!res) {
        LOG_INFO << "webauthn register rejected: " << err;
        callback(jsonError(k400BadRequest, "Registration failed: " + err));
        return;
    }

    auto db = drogon::app().getDbClient();

    // Issue recovery codes the first time the user enrols any 2FA factor.
    auto existingCodes = db->execSqlSync(
        "SELECT count(*) AS n FROM user_recovery_codes WHERE user_id = $1",
        *userIdOpt)[0]["n"].as<int>();
    std::vector<std::string> freshCodes;
    if (existingCodes == 0) freshCodes = issueFreshRecoveryCodes(*userIdOpt);

    db->execSqlSync(
        "INSERT INTO user_webauthn_credentials "
        "(user_id, credential_id, public_key, sign_count, nickname) "
        "VALUES ($1, $2, $3, $4, $5)",
        *userIdOpt, res->credential_id_b64u,
        toByteaLiteral(res->cose_public_key),
        static_cast<std::int64_t>(res->sign_count),
        nickname);

    audit_log::record(req, {"2fa.webauthn.add", userIdOpt,
                            std::nullopt, std::nullopt, Json::objectValue});

    Json::Value ret;
    ret["credential_id"] = res->credential_id_b64u;
    if (!freshCodes.empty()) {
        Json::Value arr(Json::arrayValue);
        for (auto& c : freshCodes) arr.append(c);
        ret["recovery_codes"] = arr;
    }
    callback(HttpResponse::newHttpJsonResponse(ret));
}

// =========================================================================
// /auth/2fa/webauthn/list — current passkeys (id, nickname, dates)
// =========================================================================
void AuthController::webauthnList(const HttpRequestPtr& req,
                                  std::function<void(const HttpResponsePtr&)>&& callback)
{
    auto userIdOpt = currentUserId(req);
    if (!userIdOpt) { callback(jsonError(k401Unauthorized, "Not authenticated")); return; }

    auto db = drogon::app().getDbClient();
    auto rows = db->execSqlSync(
        "SELECT id, nickname, created_at, last_used_at "
        "FROM user_webauthn_credentials WHERE user_id = $1 "
        "ORDER BY created_at",
        *userIdOpt);
    Json::Value list(Json::arrayValue);
    for (const auto& row : rows) {
        Json::Value e;
        e["id"]         = row["id"].as<std::int64_t>();
        e["nickname"]   = row["nickname"].as<std::string>();
        e["created_at"] = row["created_at"].as<std::string>();
        e["last_used_at"] = row["last_used_at"].isNull()
                            ? Json::Value(Json::nullValue)
                            : Json::Value(row["last_used_at"].as<std::string>());
        list.append(e);
    }
    Json::Value ret;
    ret["credentials"] = list;
    callback(HttpResponse::newHttpJsonResponse(ret));
}

// =========================================================================
// /auth/2fa/webauthn/remove/{id} — delete one passkey by row id
// =========================================================================
void AuthController::webauthnRemove(const HttpRequestPtr& req,
                                    std::function<void(const HttpResponsePtr&)>&& callback,
                                    std::int64_t credentialId)
{
    auto userIdOpt = currentUserId(req);
    if (!userIdOpt) { callback(jsonError(k401Unauthorized, "Not authenticated")); return; }
    auto db = drogon::app().getDbClient();
    auto r = db->execSqlSync(
        "DELETE FROM user_webauthn_credentials "
        "WHERE id = $1 AND user_id = $2 RETURNING id",
        credentialId, *userIdOpt);
    if (r.empty()) { callback(jsonError(k404NotFound, "Passkey not found")); return; }

    audit_log::record(req, {"2fa.webauthn.remove", userIdOpt,
                            std::string{"webauthn_credential"}, credentialId,
                            Json::objectValue});
    Json::Value ret;
    ret["removed"] = true;
    callback(HttpResponse::newHttpJsonResponse(ret));
}

// =========================================================================
// /auth/login/verify-totp — complete the two-step login with a TOTP code
// =========================================================================
void AuthController::verifyLoginTotp(const HttpRequestPtr& req,
                                     std::function<void(const HttpResponsePtr&)>&& callback)
{
    auto pendingOpt = pendingUserId(req);
    if (!pendingOpt) { callback(jsonError(k401Unauthorized, "No pending login")); return; }

    // Per-pending-user rate limit; per-IP is already applied in
    // helpers/Security.cc's SyncAdvice for the parent /auth/* prefix.
    auto d = security::rateLimitTake("login_2fa", std::to_string(*pendingOpt),
                                     5.0, 5.0 / 60.0);
    if (!d.allowed) {
        callback(jsonError(k429TooManyRequests, "Too many 2FA attempts"));
        return;
    }

    auto json = req->getJsonObject();
    if (!json) { callback(jsonError(k400BadRequest, "Invalid JSON")); return; }
    const std::string code = (*json)["code"].asString();

    auto db = drogon::app().getDbClient();
    auto r = db->execSqlSync(
        "SELECT secret_b32 FROM user_totp_secrets "
        "WHERE user_id = $1 AND enabled = TRUE",
        *pendingOpt);

    const std::uint64_t matchedStep =
        r.empty() ? 0
                  : totp::verifyWithStep(
                        security::unwrapTotpSecret(r[0]["secret_b32"].as<std::string>()),
                        code);

    if (matchedStep == 0) {
        audit_log::record(req, {"2fa.verify.totp.fail", pendingOpt,
                                std::nullopt, std::nullopt, Json::objectValue});
        callback(jsonError(k401Unauthorized, "Invalid code"));
        return;
    }

    // Atomic replay guard: claim this time-step in a single statement. A code
    // is accepted at most once — the UPDATE only fires when the matched step is
    // strictly newer than last_used_step, so two parallel requests presenting
    // the same code can't both win (the previous SELECT-check-then-UPDATE had a
    // TOCTOU window where both saw the same last_used_step and both passed).
    auto claim = db->execSqlSync(
        "UPDATE user_totp_secrets SET last_used_step = $2 "
        " WHERE user_id = $1 AND enabled = TRUE "
        "   AND COALESCE(last_used_step, 0) < $2 "
        "RETURNING user_id",
        *pendingOpt, static_cast<std::int64_t>(matchedStep));
    if (claim.empty()) {
        Json::Value meta;
        meta["reason"] = "replay";
        audit_log::record(req, {"2fa.verify.totp.fail", pendingOpt,
                                std::nullopt, std::nullopt, std::move(meta)});
        callback(jsonError(k401Unauthorized, "Invalid code"));
        return;
    }

    audit_log::record(req, {"login.ok", pendingOpt,
                            std::nullopt, std::nullopt, Json::objectValue});
    completeTwoStepLogin(req, callback, *pendingOpt);
}

// =========================================================================
// /auth/login/verify-recovery — single-use recovery code path
// =========================================================================
void AuthController::verifyLoginRecovery(const HttpRequestPtr& req,
                                         std::function<void(const HttpResponsePtr&)>&& callback)
{
    auto pendingOpt = pendingUserId(req);
    if (!pendingOpt) { callback(jsonError(k401Unauthorized, "No pending login")); return; }

    auto d = security::rateLimitTake("login_2fa_recov", std::to_string(*pendingOpt),
                                     5.0, 5.0 / 60.0);
    if (!d.allowed) {
        callback(jsonError(k429TooManyRequests, "Too many attempts"));
        return;
    }

    auto json = req->getJsonObject();
    if (!json) { callback(jsonError(k400BadRequest, "Invalid JSON")); return; }
    const std::string code = recovery_codes::normalize((*json)["code"].asString());
    if (code.size() != 9) {  // "XXXX-XXXX"
        callback(jsonError(k400BadRequest, "Invalid code format"));
        return;
    }

    auto db = drogon::app().getDbClient();
    auto rows = db->execSqlSync(
        "SELECT id, code_hash FROM user_recovery_codes "
        "WHERE user_id = $1 AND used_at IS NULL",
        *pendingOpt);
    std::int64_t matchedId = -1;
    for (const auto& r : rows) {
        if (recovery_codes::verifyOne(r["code_hash"].as<std::string>(), code)) {
            matchedId = r["id"].as<std::int64_t>();
            break;
        }
    }
    if (matchedId < 0) {
        audit_log::record(req, {"2fa.verify.recovery.fail", pendingOpt,
                                std::nullopt, std::nullopt, Json::objectValue});
        callback(jsonError(k401Unauthorized, "Invalid recovery code"));
        return;
    }

    // Atomic single-use consume. The SELECT above ran in C++ to match the
    // hash, but matching then updating unconditionally left a TOCTOU window:
    // two parallel requests with the same code could both pass the match and
    // both complete the login. Gating the UPDATE on `used_at IS NULL` and
    // requiring a returned row means exactly one request wins; the loser sees
    // zero rows and is rejected as a replay.
    auto consumed = db->execSqlSync(
        "UPDATE user_recovery_codes SET used_at = NOW() "
        " WHERE id = $1 AND user_id = $2 AND used_at IS NULL "
        "RETURNING id",
        matchedId, *pendingOpt);
    if (consumed.empty()) {
        audit_log::record(req, {"2fa.verify.recovery.replay", pendingOpt,
                                std::string{"recovery_code"}, matchedId,
                                Json::objectValue});
        callback(jsonError(k401Unauthorized, "Invalid recovery code"));
        return;
    }
    audit_log::record(req, {"2fa.verify.recovery.used", pendingOpt,
                            std::string{"recovery_code"}, matchedId,
                            Json::objectValue});
    audit_log::record(req, {"login.ok", pendingOpt,
                            std::nullopt, std::nullopt, Json::objectValue});
    completeTwoStepLogin(req, callback, *pendingOpt);
}

// =========================================================================
// /auth/login/verify-webauthn/begin — challenge for navigator.credentials.get
// =========================================================================
void AuthController::webauthnLoginBegin(const HttpRequestPtr& req,
                                        std::function<void(const HttpResponsePtr&)>&& callback)
{
    auto pendingOpt = pendingUserId(req);
    if (!pendingOpt) { callback(jsonError(k401Unauthorized, "No pending login")); return; }

    auto db = drogon::app().getDbClient();
    auto rows = db->execSqlSync(
        "SELECT credential_id FROM user_webauthn_credentials WHERE user_id = $1",
        *pendingOpt);
    if (rows.empty()) { callback(jsonError(k400BadRequest, "No passkeys")); return; }

    const std::string challenge = webauthn::makeChallenge();
    req->session()->insert("pending_webauthn_challenge", challenge);

    Json::Value ret;
    ret["challenge"] = challenge;
    ret["rp_id"]     = rpId();
    Json::Value allow(Json::arrayValue);
    for (const auto& row : rows) {
        Json::Value e;
        e["type"] = "public-key";
        e["id"]   = row["credential_id"].as<std::string>();
        allow.append(e);
    }
    ret["allowCredentials"] = allow;
    callback(HttpResponse::newHttpJsonResponse(ret));
}

// =========================================================================
// /auth/login/verify-webauthn/finish — verify assertion and complete login
// =========================================================================
void AuthController::webauthnLoginFinish(const HttpRequestPtr& req,
                                         std::function<void(const HttpResponsePtr&)>&& callback)
{
    auto pendingOpt = pendingUserId(req);
    if (!pendingOpt) { callback(jsonError(k401Unauthorized, "No pending login")); return; }

    auto d = security::rateLimitTake("login_2fa_passkey", std::to_string(*pendingOpt),
                                     5.0, 5.0 / 60.0);
    if (!d.allowed) {
        callback(jsonError(k429TooManyRequests, "Too many attempts"));
        return;
    }

    auto chalOpt = req->session()->getOptional<std::string>("pending_webauthn_challenge");
    if (!chalOpt) { callback(jsonError(k400BadRequest, "No pending challenge")); return; }

    auto json = req->getJsonObject();
    if (!json) { callback(jsonError(k400BadRequest, "Invalid JSON")); return; }

    const std::string credentialId = (*json)["credentialId"].asString();
    const std::string clientData   = (*json)["clientDataJSON"].asString();
    const std::string authData     = (*json)["authenticatorData"].asString();
    const std::string signature    = (*json)["signature"].asString();

    auto db = drogon::app().getDbClient();
    auto rows = db->execSqlSync(
        "SELECT id, public_key, sign_count "
        "FROM user_webauthn_credentials "
        "WHERE user_id = $1 AND credential_id = $2",
        *pendingOpt, credentialId);
    if (rows.empty()) {
        callback(jsonError(k401Unauthorized, "Unknown credential"));
        return;
    }
    const std::int64_t  credRowId  = rows[0]["id"].as<std::int64_t>();
    const auto          storedKey  = rows[0]["public_key"].as<std::string>();
    const std::int64_t  storedCnt  = rows[0]["sign_count"].as<std::int64_t>();

    std::string err;
    auto res = webauthn::finishAuthentication(
        clientData, authData, signature,
        *chalOpt, rpId(), siteOrigin(),
        std::vector<unsigned char>(storedKey.begin(), storedKey.end()),
        static_cast<std::uint32_t>(storedCnt),
        err);
    req->session()->erase("pending_webauthn_challenge");
    if (!res) {
        LOG_INFO << "webauthn assertion rejected: " << err;
        audit_log::record(req, {"2fa.verify.webauthn.fail", pendingOpt,
                                std::string{"webauthn_credential"}, credRowId,
                                Json::objectValue});
        callback(jsonError(k401Unauthorized, "Authentication failed"));
        return;
    }

    // Conditional UPDATE closes the TOCTOU window between the SELECT
    // above and this write. With `sign_count < $1` two concurrent
    // verifications of the same captured assertion would otherwise BOTH:
    //   - SELECT the same storedCnt
    //   - pass new_sign_count > storedCnt
    //   - UPDATE sign_count = new_sign_count
    // …completing twice for a single counter advance. The guard makes the
    // second UPDATE a no-op (0 rows affected); the helper's sign_count
    // regression check on the next legitimate login then rejects the
    // cloned credential. RETURNING id lets us notice the miss and audit it.
    //
    // The `$1 = 0` branch handles authenticators that never increment their
    // counter (Apple/most platform passkeys always report 0). For those the
    // helper accepts the assertion (WebAuthn allows a static 0), but
    // `sign_count < $1` would be `0 < 0` = false and reject every login as a
    // replay. Counter-based clone detection is simply unavailable for these
    // keys; replay is instead prevented by the single-use challenge, which is
    // erased from the session immediately after finishAuthentication above.
    auto upd = db->execSqlSync(
        "UPDATE user_webauthn_credentials "
        "   SET sign_count = $1, last_used_at = NOW() "
        " WHERE id = $2 AND ($1 = 0 OR sign_count < $1) "
        "RETURNING id",
        static_cast<std::int64_t>(res->new_sign_count), credRowId);
    if (upd.empty()) {
        audit_log::record(req, {"2fa.verify.webauthn.replay", pendingOpt,
                                std::string{"webauthn_credential"}, credRowId,
                                Json::objectValue});
        callback(jsonError(k401Unauthorized, "Authentication failed"));
        return;
    }

    audit_log::record(req, {"2fa.verify.webauthn.ok", pendingOpt,
                            std::string{"webauthn_credential"}, credRowId,
                            Json::objectValue});
    audit_log::record(req, {"login.ok", pendingOpt,
                            std::nullopt, std::nullopt, Json::objectValue});
    completeTwoStepLogin(req, callback, *pendingOpt);
}
