#include "AuthController.h"
#include "../models/Users.h"
#include "../models/PasswordResetTokens.h"
#include "../helpers/AuditLog.h"
#include "../helpers/EmailHelper.h"
#include "../helpers/HttpCache.h"
#include "../helpers/Security.h"
#include "../helpers/Workers.h"

#include <drogon/orm/Mapper.h>
#include <trantor/utils/Logger.h>
#include <sodium.h>

#include <stdexcept>
#include <string>

using namespace drogon;
using namespace drogon::orm;

namespace {

using security::hashPassword;
using security::verifyPassword;

// Static dummy Argon2id hash computed once at first use. Used so that login
// attempts against non-existent usernames still spend the same Argon2id cost
// as real verifications, closing the user-enumeration timing side-channel.
const std::string& dummyHash()
{
    static const std::string h = hashPassword("not-a-real-password-just-for-timing");
    return h;
}

// CSRF cookie issuance moved to security::issueCsrfCookie (helpers/Security)
// so the two-step (2FA) login completion in AuthController2fa.cc can emit it
// too — see fix for the post-2FA "first mutating request 403s" flow bug.
using security::issueCsrfCookie;

constexpr std::size_t kMinPasswordLen = 8;
constexpr std::size_t kMaxPasswordLen = 256;
constexpr std::size_t kMinUsernameLen = 3;
constexpr std::size_t kMaxUsernameLen = 32;
constexpr std::size_t kMaxEmailLen    = 255;

bool passwordTooWeak(const std::string& p)
{
    return p.size() < kMinPasswordLen || p.size() > kMaxPasswordLen;
}

// Restrict usernames to a safe, predictable charset. Without this a username
// could carry spaces, control characters, or unicode homoglyphs — enabling
// visual impersonation (admin vs аdmin) and messy data downstream. Vue already
// escapes on render so this is not an XSS fix; it is anti-spoofing + data
// hygiene. Only enforced at registration, so existing accounts are unaffected.
bool usernameValid(const std::string& u)
{
    if (u.size() < kMinUsernameLen || u.size() > kMaxUsernameLen) return false;
    for (unsigned char c : u) {
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                        (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok) return false;
    }
    return true;
}

// Email-shape sanity check moved to helpers/Security.h so the same
// validator runs on every endpoint that touches an outbound SMTP
// To: header (registration, profile update). Local alias kept so the
// caller sites read naturally.
using security::emailLooksValid;

HttpResponsePtr jsonError(HttpStatusCode code, const std::string& msg)
{
    Json::Value body;
    body["error"] = msg;
    auto resp = HttpResponse::newHttpJsonResponse(body);
    resp->setStatusCode(code);
    return resp;
}

} // namespace

void AuthController::registerUser(const HttpRequestPtr &req,
                                  std::function<void(const HttpResponsePtr &)> &&callback)
{
    auto json = req->getJsonObject();
    if (!json) { callback(jsonError(k400BadRequest, "Invalid JSON")); return; }

    std::string username = (*json)["username"].asString();
    std::string email    = (*json)["email"].asString();
    std::string password = (*json)["password"].asString();

    if (username.empty() || email.empty() || password.empty()) {
        callback(jsonError(k400BadRequest, "All fields are required"));
        return;
    }
    if (email.size() > kMaxEmailLen) {
        callback(jsonError(k400BadRequest, "Field too long"));
        return;
    }
    if (!usernameValid(username)) {
        callback(jsonError(k400BadRequest,
            "Username must be 3-32 characters: letters, digits, _ or -"));
        return;
    }
    if (!emailLooksValid(email)) {
        // Catches CR/LF / spaces / missing @ etc. — important because
        // the address ends up in the SMTP To: header downstream and a
        // newline there would let an attacker chain Bcc: / extra
        // headers (CWE-93 email header injection).
        callback(jsonError(k400BadRequest, "Invalid email address"));
        return;
    }
    if (passwordTooWeak(password)) {
        callback(jsonError(k400BadRequest,
                           "Password must be at least 8 characters"));
        return;
    }

    // Registration hashes with Argon2id — twice on the masked-collision
    // path, since the throwaway hash is what keeps the two outcomes
    // indistinguishable — plus synchronous ORM lookups and an insert. All
    // of it blocks, so none of it belongs on an IO loop.
    workers::offload(workers::Pool::Auth, callback,
        [req, callback, username, email, password] {

    auto dbClient = drogon::app().getDbClient();
    Mapper<drogon_model::blog_db::Users> mapper(dbClient);

    // Always-the-same JSON response we hand back so neither an email collision
    // nor a fresh registration is distinguishable from the outside.
    auto successResp = []() {
        Json::Value ret;
        ret["message"]    = "Registration successful. Please check your email to verify your account.";
        ret["email_sent"] = true;
        auto resp = HttpResponse::newHttpJsonResponse(ret);
        resp->setStatusCode(k201Created);
        return resp;
    };

    try {
        // Username is public (visible on profiles, post authors, …) — leaking
        // its existence is unavoidable, so we keep the explicit 409.
        auto byUsername = mapper.findBy(
            Criteria(drogon_model::blog_db::Users::Cols::_username,
                     CompareOperator::EQ, username));
        if (!byUsername.empty()) {
            callback(jsonError(k409Conflict, "Username already taken"));
            return;
        }

        // Email collisions are silently masked: we return success and notify
        // the legitimate owner out-of-band. The attacker can't tell whether
        // we created a new account or not.
        //
        // We run hashPassword on this path even though we throw the hash
        // away — without it the dup-email reply lands ~135 ms ahead of
        // the new-account reply (Argon2id at OPSLIMIT_INTERACTIVE), an
        // obvious timing oracle for account enumeration.
        auto byEmail = mapper.findBy(
            Criteria(drogon_model::blog_db::Users::Cols::_email,
                     CompareOperator::EQ, email));
        if (!byEmail.empty()) {
            (void)hashPassword(password);
            EmailHelper::sendRegistrationAttemptEmail(
                email, byEmail[0].getValueOfUsername());
            callback(successResp());
            return;
        }
    } catch (const DrogonDbException &e) {
        LOG_ERROR << "DB Error (register pre-check): " << e.base().what();
        callback(jsonError(k500InternalServerError, "Registration failed"));
        return;
    }

    drogon_model::blog_db::Users newUser;
    newUser.setUsername(username);
    newUser.setEmail(email);
    newUser.setPasswordHash(hashPassword(password));
    newUser.setBio("");
    newUser.setEmailVerified(0);

    // Store SHA-256 of the verification token, not the raw value. A DB
    // snapshot won't leak active tokens; the plaintext only exists on
    // the wire to the user's mailbox and in the verify endpoint's
    // request body. Same pattern is used for password_reset_tokens
    // further down. The token is base64-url so each char carries
    // ~6 bits — 32 chars is ~192 bits of entropy, well past brute force.
    std::string verificationToken = EmailHelper::generateToken();
    newUser.setEmailVerificationToken(security::sha256Hex(verificationToken));
    newUser.setEmailVerificationExpires(trantor::Date::now().after(24 * 3600));

    try {
        mapper.insert(newUser);
        audit_log::record(req, {"register",
                                static_cast<int>(newUser.getValueOfId()),
                                std::nullopt, std::nullopt, Json::objectValue});
        EmailHelper::sendVerificationEmail(email, username, verificationToken);
        callback(successResp());
    } catch (const DrogonDbException &e) {
        // Unique-constraint races: another tx grabbed the username/email
        // between our pre-check and the insert. Treat both as collisions —
        // the email branch keeps masking, the username branch surfaces.
        LOG_ERROR << "DB Error (register insert): " << e.base().what();
        callback(jsonError(k409Conflict, "Username already taken"));
    }

        });
}

void AuthController::loginUser(const HttpRequestPtr &req,
                               std::function<void(const HttpResponsePtr &)> &&callback)
{
    auto json = req->getJsonObject();
    if (!json) { callback(jsonError(k400BadRequest, "Invalid JSON")); return; }

    std::string username = (*json)["username"].asString();
    std::string password = (*json)["password"].asString();

    if (username.empty() || password.empty()) {
        callback(jsonError(k400BadRequest,
                           "Username and password are required"));
        return;
    }

    // Per-username rate limit (independent of the per-IP limit applied in the
    // SyncAdvice). Defeats credential stuffing where an attacker rotates IPs
    // against a single account.
    {
        const char* disabled = std::getenv("BLOG_DISABLE_RATE_LIMIT");
        const bool rlOn = !(disabled && std::string(disabled) == "1");
        if (rlOn) {
            auto d = security::rateLimitTake("login_user", username,
                                             10.0, 10.0 / 600.0); // 10 burst, 10/10min
            if (!d.allowed) {
                auto resp = jsonError(k429TooManyRequests,
                                      "Too many attempts on this account");
                resp->addHeader("Retry-After",
                    std::to_string(static_cast<int>(d.retryAfterSeconds) + 1));
                callback(resp);
                return;
            }
        }
    }

    // Everything below blocks: the ORM lookup is synchronous, the Argon2id
    // verify costs ~158 ms on this host by design, and the 2FA enrolment
    // check is two more synchronous round-trips. Run on this IO loop it
    // would stall every other connection Drogon assigned to the same loop
    // for the whole login. The rate-limit checks above stay on the loop so
    // a shed request never occupies a worker slot.
    workers::offload(workers::Pool::Auth, callback,
        [req, callback, username, password] {

    auto dbClient = drogon::app().getDbClient();
    Mapper<drogon_model::blog_db::Users> mapper(dbClient);

    try {
        auto users = mapper.findBy(
            Criteria(drogon_model::blog_db::Users::Cols::_username,
                     CompareOperator::EQ, username));

        // Constant-time path: verify against either the real hash or a dummy
        // hash so the response latency does not leak account existence.
        const bool exists  = !users.empty();
        const std::string& hash = exists ? users[0].getValueOfPasswordHash()
                                         : dummyHash();
        const bool valid = verifyPassword(hash, password) && exists;

        if (!valid) {
            // Audit failed attempt. metadata.username is intentionally the
            // *attempted* username, which may be made up — we want to spot
            // patterns of probing, not just hits on real accounts.
            Json::Value meta;
            meta["username"]      = username;
            meta["account_found"] = exists;
            audit_log::record(req, {"login.fail", std::nullopt,
                                    std::nullopt, std::nullopt, std::move(meta)});
            callback(jsonError(k401Unauthorized, "Invalid credentials"));
            return;
        }

        const auto& user = users[0];
        const int  userId = static_cast<int>(user.getValueOfId());

        // Look up 2FA enrolment. The query is synchronous on purpose —
        // simpler than building an async chain when we already hold the
        // hot path for the password check, and findOne returns instantly
        // for a single-row indexed lookup.
        bool has2fa = false;
        bool totpEnabled = false;
        int  passkeysCount = 0;
        {
            auto r = dbClient->execSqlSync(
                "SELECT enabled FROM user_totp_secrets WHERE user_id = $1", userId);
            if (!r.empty()) totpEnabled = r[0]["enabled"].as<bool>();

            auto r2 = dbClient->execSqlSync(
                "SELECT count(*) AS n FROM user_webauthn_credentials WHERE user_id = $1",
                userId);
            passkeysCount = r2[0]["n"].as<int>();
            has2fa = totpEnabled || passkeysCount > 0;
        }

        // Always rotate the session ID after a successful password check —
        // covers fixation in the no-2FA case AND prevents an attacker from
        // landing pre-2FA pending state into a victim's session.
        auto session = req->session();
        session->clear();
        session->changeSessionIdToClient();

        if (has2fa) {
            // Two-step gate: stash a pending_user_id but DO NOT set user_id.
            // Anything reading the session before /auth/login/verify-* runs
            // will see an unauthenticated state.
            session->insert("pending_user_id", userId);

            Json::Value ret;
            ret["requires_2fa"] = true;
            Json::Value methods(Json::arrayValue);
            if (totpEnabled)       methods.append("totp");
            if (passkeysCount > 0) methods.append("webauthn");
            methods.append("recovery");
            ret["methods"] = methods;

            audit_log::record(req, {"login.password_ok",
                                    userId, std::nullopt, std::nullopt,
                                    Json::objectValue});
            callback(HttpResponse::newHttpJsonResponse(ret));
            return;
        }

        session->insert("user_id",  userId);
        session->insert("username", user.getValueOfUsername());

        audit_log::record(req, {"login.ok", userId,
                                std::nullopt, std::nullopt, Json::objectValue});

        Json::Value ret;
        ret["message"]          = "Login successful";
        ret["user"]["id"]       = user.getValueOfId();
        ret["user"]["username"] = user.getValueOfUsername();
        ret["user"]["email"]    = user.getValueOfEmail();

        auto resp = HttpResponse::newHttpJsonResponse(ret);
        issueCsrfCookie(req, resp);
        callback(resp);
    } catch (const DrogonDbException &e) {
        LOG_ERROR << "DB Error (login): " << e.base().what();
        callback(jsonError(k500InternalServerError, "Login failed"));
    }

        });
}

void AuthController::logoutUser(const HttpRequestPtr &req,
                                std::function<void(const HttpResponsePtr &)> &&callback)
{
    auto session = req->session();
    auto userIdOpt = session->getOptional<int>("user_id");

    // Wipe everything from the session and rotate the ID so any captured
    // cookie is useless after this call.
    session->clear();
    session->changeSessionIdToClient();

    audit_log::record(req, {"logout", userIdOpt,
                            std::nullopt, std::nullopt, Json::objectValue});

    Json::Value ret;
    ret["message"] = "Logout successful";
    callback(HttpResponse::newHttpJsonResponse(ret));
}

void AuthController::getCurrentUser(const HttpRequestPtr &req,
                                   std::function<void(const HttpResponsePtr &)> &&callback)
{
    auto session = req->session();
    auto userIdOpt = session->getOptional<int>("user_id");

    if (!userIdOpt.has_value()) {
        callback(jsonError(k401Unauthorized, "Not authenticated"));
        return;
    }

    auto dbClient = drogon::app().getDbClient();
    Mapper<drogon_model::blog_db::Users> mapper(dbClient);

    try {
        auto user = mapper.findByPrimaryKey(userIdOpt.value());

        // ETag derives from (id, updated_at). Any profile mutation goes
        // through users.updated_at (BEFORE-UPDATE trigger from 0001_init);
        // the CSRF cookie rehydration below does not change the response
        // body, only Set-Cookie, so it doesn't need to invalidate.
        //
        // Vary: Cookie is the load-bearing header here: /auth/me returns
        // identity scoped to the session cookie. Without Vary, any cache
        // (browser back/forward cache, future Cloudflare-edge cache,
        // intermediate corporate proxy) could serve one user's response
        // to the next request from a different cookie.
        const std::string etag = http_cache::makeWeakEtag({
            std::to_string(user.getValueOfId()),
            std::to_string(http_cache::parseTimestampMicros(
                user.getValueOfUpdatedAt().toDbStringLocal())),
        });
        constexpr std::string_view kVary = "Cookie";

        if (http_cache::ifNoneMatchHit(req, etag)) {
            callback(http_cache::makeNotModified(etag, 0, kVary));
            return;
        }

        Json::Value ret;
        ret["id"]       = user.getValueOfId();
        ret["username"] = user.getValueOfUsername();
        ret["email"]    = user.getValueOfEmail();
        ret["bio"]      = user.getValueOfBio();
        if (!user.getValueOfProfileImage().empty()) {
            ret["profile_image"] = user.getValueOfProfileImage();
        }

        auto resp = HttpResponse::newHttpJsonResponse(ret);
        issueCsrfCookie(req, resp);             // rehydrate CSRF on session bootstrap
        http_cache::applyCacheHeaders(resp, etag, 0, kVary);
        callback(resp);
    } catch (const DrogonDbException &e) {
        LOG_ERROR << "DB Error (me): " << e.base().what();
        callback(jsonError(k404NotFound, "User not found"));
    }
}

void AuthController::verifyEmail(const HttpRequestPtr &req,
                                 std::function<void(const HttpResponsePtr &)> &&callback)
{
    auto json = req->getJsonObject();
    if (!json) { callback(jsonError(k400BadRequest, "Invalid JSON")); return; }

    std::string token = (*json)["token"].asString();
    if (token.empty() || token.size() > 128) {
        callback(jsonError(k400BadRequest, "Token is required"));
        return;
    }

    auto dbClient = drogon::app().getDbClient();

    // Atomic consume: the row is only touched if the token is still valid AND
    // not expired. Two parallel calls cannot both succeed because only the
    // first one matches the where-clause; the second sees 0 affected rows.
    static const char* kSql =
        "UPDATE users "
        "   SET email_verified = 1, "
        "       email_verification_token = NULL, "
        "       email_verification_expires = NULL "
        " WHERE email_verification_token = $1 "
        "   AND email_verification_expires > NOW() "
        "RETURNING id";

    // Compare against the stored hash, not the plaintext — the DB only
    // ever holds the SHA-256 since registration. See the matching write
    // in registerUser() for the rationale.
    dbClient->execSqlAsync(
        kSql,
        [callback, req](const Result& r) {
            if (r.empty()) {
                callback(jsonError(k400BadRequest, "Invalid or expired token"));
                return;
            }
            const int userId = r[0]["id"].as<int>();
            audit_log::record(req, {"email.verify", userId,
                                    std::nullopt, std::nullopt, Json::objectValue});
            Json::Value ret;
            ret["message"] = "Email verified successfully";
            callback(HttpResponse::newHttpJsonResponse(ret));
        },
        [callback](const DrogonDbException& e) {
            LOG_ERROR << "DB Error (verifyEmail): " << e.base().what();
            callback(jsonError(k500InternalServerError, "Verification failed"));
        },
        security::sha256Hex(token));
}

void AuthController::requestPasswordReset(const HttpRequestPtr &req,
                                         std::function<void(const HttpResponsePtr &)> &&callback)
{
    auto json = req->getJsonObject();
    if (!json) { callback(jsonError(k400BadRequest, "Invalid JSON")); return; }

    std::string email = (*json)["email"].asString();
    if (email.empty() || email.size() > kMaxEmailLen ||
        !emailLooksValid(email))
    {
        // Defence-in-depth: registration already filters CR/LF / space
        // before any value lands in users.email, so the DB lookup below
        // can never match a header-injection attempt. Validating here
        // still catches obviously-malformed inputs early and keeps the
        // SMTP To: header safe even if a future write path skips the
        // registration gate.
        callback(jsonError(k400BadRequest, "Email is required"));
        return;
    }

    // Same response in both branches so the email's existence stays opaque.
    // Captureless so it remains safe to copy into async lambdas.
    auto okResp = []() {
        Json::Value ret;
        ret["message"] = "If an account exists with that email, a password reset link has been sent.";
        return HttpResponse::newHttpJsonResponse(ret);
    };

    auto dbClient = drogon::app().getDbClient();
    Mapper<drogon_model::blog_db::Users> userMapper(dbClient);

    try {
        auto users = userMapper.findBy(
            Criteria(drogon_model::blog_db::Users::Cols::_email,
                     CompareOperator::EQ, email));
        if (users.empty()) {
            callback(okResp());
            return;
        }

        const auto& user = users[0];
        const auto userId = static_cast<int>(user.getValueOfId());

        audit_log::record(req, {"password.reset.request", userId,
                                std::nullopt, std::nullopt, Json::objectValue});

        // Invalidate any reset tokens still outstanding for this user — a fresh
        // request supersedes earlier ones to prevent stolen-mailbox replay.
        dbClient->execSqlAsync(
            "DELETE FROM password_reset_tokens WHERE user_id = $1",
            [dbClient, userId, email, username = user.getValueOfUsername(), okResp, callback]
            (const Result&) {
                // Plaintext goes out to the user's mailbox; the DB only
                // stores the SHA-256 so a snapshot can't replay the
                // token. resetPassword() rehashes the inbound token
                // for the same reason.
                const std::string resetToken = EmailHelper::generateToken();
                const std::string resetTokenHash = security::sha256Hex(resetToken);
                dbClient->execSqlAsync(
                    "INSERT INTO password_reset_tokens (user_id, token, expires_at) "
                    "VALUES ($1, $2, NOW() + INTERVAL '1 hour')",
                    [email, username, resetToken, okResp, callback]
                    (const Result&) {
                        EmailHelper::sendPasswordResetEmail(email, username, resetToken);
                        callback(okResp());
                    },
                    [okResp, callback](const DrogonDbException& e) {
                        LOG_ERROR << "DB Error (reset insert): " << e.base().what();
                        // Still return the masked success — never leak email
                        // state on infra errors.
                        callback(okResp());
                    },
                    userId, resetTokenHash);
            },
            [okResp, callback](const DrogonDbException& e) {
                LOG_ERROR << "DB Error (reset wipe): " << e.base().what();
                callback(okResp());
            },
            userId);
    } catch (const DrogonDbException &e) {
        LOG_ERROR << "DB Error (request reset): " << e.base().what();
        callback(okResp());
    }
}

void AuthController::resetPassword(const HttpRequestPtr &req,
                                  std::function<void(const HttpResponsePtr &)> &&callback)
{
    auto json = req->getJsonObject();
    if (!json) { callback(jsonError(k400BadRequest, "Invalid JSON")); return; }

    std::string token       = (*json)["token"].asString();
    std::string newPassword = (*json)["password"].asString();
    if (token.empty() || newPassword.empty()) {
        callback(jsonError(k400BadRequest, "Token and password are required"));
        return;
    }
    if (passwordTooWeak(newPassword)) {
        callback(jsonError(k400BadRequest,
                           "Password must be at least 8 characters"));
        return;
    }

    // Atomic token consumption: a successful DELETE returns the user_id, after
    // which we can't fail the password update without leaving the token gone.
    // Two parallel requests with the same token: only one DELETE returns rows.
    auto dbClient = drogon::app().getDbClient();
    dbClient->execSqlAsync(
        "DELETE FROM password_reset_tokens "
        " WHERE token = $1 AND expires_at > NOW() "
        "RETURNING user_id",
        [dbClient, newPassword, callback, req](const Result& r) {
            if (r.empty()) {
                callback(jsonError(k400BadRequest, "Invalid or expired token"));
                return;
            }
            const int userId = r[0]["user_id"].as<int>();

            // This callback runs on the database connection's own thread,
            // not an IO loop, but hashing here still parks that connection
            // for ~167 ms and every query queued behind it waits. Hand the
            // Argon2id work to the auth pool and let the connection go.
            workers::offload(workers::Pool::Auth, callback,
                [dbClient, newPassword, callback, req, userId] {
                    const std::string hash = hashPassword(newPassword);

                    dbClient->execSqlAsync(
                        "UPDATE users SET password_hash = $1 WHERE id = $2",
                        [callback, req, userId](const Result&) {
                            audit_log::record(req, {"password.reset", userId,
                                                    std::nullopt, std::nullopt,
                                                    Json::objectValue});
                            Json::Value ret;
                            ret["message"] = "Password reset successfully";
                            callback(HttpResponse::newHttpJsonResponse(ret));
                        },
                        [callback](const DrogonDbException& e) {
                            LOG_ERROR << "DB Error (reset update): "
                                      << e.base().what();
                            callback(jsonError(k500InternalServerError,
                                               "Failed to reset password"));
                        },
                        hash, userId);
                });
        },
        [callback](const DrogonDbException& e) {
            LOG_ERROR << "DB Error (reset consume): " << e.base().what();
            callback(jsonError(k500InternalServerError,
                               "Failed to reset password"));
        },
        security::sha256Hex(token));
}

void AuthController::resendVerification(const HttpRequestPtr &req,
                                       std::function<void(const HttpResponsePtr &)> &&callback)
{
    auto json = req->getJsonObject();
    if (!json) { callback(jsonError(k400BadRequest, "Invalid JSON")); return; }

    std::string email = (*json)["email"].asString();
    if (email.empty() || email.size() > kMaxEmailLen ||
        !emailLooksValid(email))
    {
        // Defence-in-depth: registration already filters CR/LF / space
        // before any value lands in users.email, so the DB lookup below
        // can never match a header-injection attempt. Validating here
        // still catches obviously-malformed inputs early and keeps the
        // SMTP To: header safe even if a future write path skips the
        // registration gate.
        callback(jsonError(k400BadRequest, "Email is required"));
        return;
    }

    // Uniform response shape: don't leak whether the account exists or is
    // already verified.
    auto okResp = []() {
        Json::Value ret;
        ret["message"] = "If a matching unverified account exists, a verification email has been sent.";
        return HttpResponse::newHttpJsonResponse(ret);
    };

    auto dbClient = drogon::app().getDbClient();
    Mapper<drogon_model::blog_db::Users> mapper(dbClient);

    try {
        auto users = mapper.findBy(
            Criteria(drogon_model::blog_db::Users::Cols::_email,
                     CompareOperator::EQ, email));

        if (users.empty() || users[0].getValueOfEmailVerified() == 1) {
            callback(okResp());
            return;
        }

        auto user = users[0];
        // Same hash-at-rest pattern as registerUser. Plaintext only
        // crosses the wire to the user's mailbox.
        const std::string verificationToken = EmailHelper::generateToken();
        user.setEmailVerificationToken(security::sha256Hex(verificationToken));
        user.setEmailVerificationExpires(trantor::Date::now().after(24 * 3600));
        mapper.update(user);

        EmailHelper::sendVerificationEmail(email, user.getValueOfUsername(),
                                           verificationToken);
        callback(okResp());
    } catch (const DrogonDbException &e) {
        LOG_ERROR << "DB Error (resend): " << e.base().what();
        callback(okResp());
    }
}
