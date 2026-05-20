#include "AuthController.h"
#include "../models/Users.h"
#include "../models/PasswordResetTokens.h"
#include "../helpers/EmailHelper.h"
#include "../helpers/Security.h"

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

// Ensures a CSRF token exists for the current session and that the response
// carries the matching readable (non-HttpOnly) Lax cookie. Idempotent: when
// the session already has a token (returning user, /auth/me), the same value
// is re-emitted so the frontend can pick it up after a reload.
void issueCsrfCookie(const drogon::HttpRequestPtr& req,
                     const drogon::HttpResponsePtr& resp)
{
    auto session = req->session();
    std::string token;
    auto existing = session->getOptional<std::string>("csrf_token");
    if (existing.has_value() && !existing.value().empty()) {
        token = existing.value();
    } else {
        token = security::randomToken();
        session->insert("csrf_token", token);
    }

    drogon::Cookie c(security::csrfCookieName(), token);
    c.setPath("/");
    c.setHttpOnly(false);                       // frontend reads it to echo in header
    c.setSameSite(drogon::Cookie::SameSite::kLax);
    c.setSecure(security::secureCookies());
    resp->addCookie(std::move(c));
}

constexpr std::size_t kMinPasswordLen = 8;
constexpr std::size_t kMaxPasswordLen = 256;
constexpr std::size_t kMaxUsernameLen = 64;
constexpr std::size_t kMaxEmailLen    = 255;

bool passwordTooWeak(const std::string& p)
{
    return p.size() < kMinPasswordLen || p.size() > kMaxPasswordLen;
}

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
    if (username.size() > kMaxUsernameLen || email.size() > kMaxEmailLen) {
        callback(jsonError(k400BadRequest, "Field too long"));
        return;
    }
    if (passwordTooWeak(password)) {
        callback(jsonError(k400BadRequest,
                           "Password must be at least 8 characters"));
        return;
    }

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
        auto byEmail = mapper.findBy(
            Criteria(drogon_model::blog_db::Users::Cols::_email,
                     CompareOperator::EQ, email));
        if (!byEmail.empty()) {
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

    std::string verificationToken = EmailHelper::generateToken();
    newUser.setEmailVerificationToken(verificationToken);
    newUser.setEmailVerificationExpires(trantor::Date::now().after(24 * 3600));

    try {
        mapper.insert(newUser);
        EmailHelper::sendVerificationEmail(email, username, verificationToken);
        callback(successResp());
    } catch (const DrogonDbException &e) {
        // Unique-constraint races: another tx grabbed the username/email
        // between our pre-check and the insert. Treat both as collisions —
        // the email branch keeps masking, the username branch surfaces.
        LOG_ERROR << "DB Error (register insert): " << e.base().what();
        callback(jsonError(k409Conflict, "Username already taken"));
    }
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
            callback(jsonError(k401Unauthorized, "Invalid credentials"));
            return;
        }

        const auto& user = users[0];

        // Defeat session fixation: drop any pre-login state and have Drogon
        // mint a new session ID before we attach the authenticated identity.
        auto session = req->session();
        session->clear();
        session->changeSessionIdToClient();
        session->insert("user_id",  static_cast<int>(user.getValueOfId()));
        session->insert("username", user.getValueOfUsername());

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
}

void AuthController::logoutUser(const HttpRequestPtr &req,
                                std::function<void(const HttpResponsePtr &)> &&callback)
{
    // Wipe everything from the session and rotate the ID so any captured
    // cookie is useless after this call.
    auto session = req->session();
    session->clear();
    session->changeSessionIdToClient();

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

    dbClient->execSqlAsync(
        kSql,
        [callback](const Result& r) {
            if (r.empty()) {
                callback(jsonError(k400BadRequest, "Invalid or expired token"));
                return;
            }
            Json::Value ret;
            ret["message"] = "Email verified successfully";
            callback(HttpResponse::newHttpJsonResponse(ret));
        },
        [callback](const DrogonDbException& e) {
            LOG_ERROR << "DB Error (verifyEmail): " << e.base().what();
            callback(jsonError(k500InternalServerError, "Verification failed"));
        },
        token);
}

void AuthController::requestPasswordReset(const HttpRequestPtr &req,
                                         std::function<void(const HttpResponsePtr &)> &&callback)
{
    auto json = req->getJsonObject();
    if (!json) { callback(jsonError(k400BadRequest, "Invalid JSON")); return; }

    std::string email = (*json)["email"].asString();
    if (email.empty() || email.size() > kMaxEmailLen) {
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

        // Invalidate any reset tokens still outstanding for this user — a fresh
        // request supersedes earlier ones to prevent stolen-mailbox replay.
        dbClient->execSqlAsync(
            "DELETE FROM password_reset_tokens WHERE user_id = $1",
            [dbClient, userId, email, username = user.getValueOfUsername(), okResp, callback]
            (const Result&) {
                const std::string resetToken = EmailHelper::generateToken();
                // We rely on the FK ON DELETE CASCADE / unique constraints in
                // the schema; this is a simple INSERT.
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
                    userId, resetToken);
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
        [dbClient, newPassword, callback](const Result& r) {
            if (r.empty()) {
                callback(jsonError(k400BadRequest, "Invalid or expired token"));
                return;
            }
            const int userId = r[0]["user_id"].as<int>();
            const std::string hash = hashPassword(newPassword);

            dbClient->execSqlAsync(
                "UPDATE users SET password_hash = $1 WHERE id = $2",
                [callback](const Result&) {
                    Json::Value ret;
                    ret["message"] = "Password reset successfully";
                    callback(HttpResponse::newHttpJsonResponse(ret));
                },
                [callback](const DrogonDbException& e) {
                    LOG_ERROR << "DB Error (reset update): " << e.base().what();
                    callback(jsonError(k500InternalServerError,
                                       "Failed to reset password"));
                },
                hash, userId);
        },
        [callback](const DrogonDbException& e) {
            LOG_ERROR << "DB Error (reset consume): " << e.base().what();
            callback(jsonError(k500InternalServerError,
                               "Failed to reset password"));
        },
        token);
}

void AuthController::resendVerification(const HttpRequestPtr &req,
                                       std::function<void(const HttpResponsePtr &)> &&callback)
{
    auto json = req->getJsonObject();
    if (!json) { callback(jsonError(k400BadRequest, "Invalid JSON")); return; }

    std::string email = (*json)["email"].asString();
    if (email.empty() || email.size() > kMaxEmailLen) {
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
        const std::string verificationToken = EmailHelper::generateToken();
        user.setEmailVerificationToken(verificationToken);
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
