#include "AuthController.h"
#include "../models/Users.h"
#include "../models/PasswordResetTokens.h"
#include "../helpers/EmailHelper.h"
#include <drogon/orm/Mapper.h>
#include <trantor/utils/Logger.h>
#include <sodium.h>
#include <stdexcept>
#include <string>

using namespace drogon;
using namespace drogon::orm;

namespace {

// Argon2id-based password hashing via libsodium. The output is a self-describing
// string (algorithm id + parameters + salt + tag), so verification only needs the
// stored hash and the candidate password — no extra salt column.
std::string hashPassword(const std::string& password)
{
    char out[crypto_pwhash_STRBYTES];
    if (crypto_pwhash_str(out,
                          password.c_str(),
                          password.size(),
                          crypto_pwhash_OPSLIMIT_INTERACTIVE,
                          crypto_pwhash_MEMLIMIT_INTERACTIVE) != 0)
    {
        throw std::runtime_error("password hashing failed");
    }
    return std::string(out);
}

bool verifyPassword(const std::string& storedHash, const std::string& candidate)
{
    return crypto_pwhash_str_verify(storedHash.c_str(),
                                    candidate.c_str(),
                                    candidate.size()) == 0;
}

} // namespace

void AuthController::registerUser(const HttpRequestPtr &req,
                                  std::function<void(const HttpResponsePtr &)> &&callback)
{
    auto json = req->getJsonObject();
    if (!json) {
        auto resp = HttpResponse::newHttpJsonResponse(
            Json::Value("error: Invalid JSON"));
        resp->setStatusCode(k400BadRequest);
        callback(resp);
        return;
    }

    std::string username = (*json)["username"].asString();
    std::string email = (*json)["email"].asString();
    std::string password = (*json)["password"].asString();

    if (username.empty() || email.empty() || password.empty()) {
        Json::Value ret;
        ret["error"] = "All fields are required";
        auto resp = HttpResponse::newHttpJsonResponse(ret);
        resp->setStatusCode(k400BadRequest);
        callback(resp);
        return;
    }

    auto dbClient = drogon::app().getDbClient();
    Mapper<drogon_model::blog_db::Users> mapper(dbClient);

    // Check if username or email already exists
    try {
        auto existingUser = mapper.findBy(
            Criteria(drogon_model::blog_db::Users::Cols::_username, 
                    CompareOperator::EQ, username) ||
            Criteria(drogon_model::blog_db::Users::Cols::_email, 
                    CompareOperator::EQ, email)
        );
        
        if (existingUser.size() > 0) {
            Json::Value ret;
            ret["error"] = "Username or email already exists";
            auto resp = HttpResponse::newHttpJsonResponse(ret);
            resp->setStatusCode(k409Conflict);
            callback(resp);
            return;
        }
    } catch (const DrogonDbException &e) {
        LOG_ERROR << "DB Error: " << e.base().what();
    }

    // Create new user
    drogon_model::blog_db::Users newUser;
    newUser.setUsername(username);
    newUser.setEmail(email);
    newUser.setPasswordHash(hashPassword(password));
    newUser.setBio("");
    newUser.setEmailVerified(0);
    
    // Generate email verification token
    std::string verificationToken = EmailHelper::generateToken();
    newUser.setEmailVerificationToken(verificationToken);
    
    // Set expiration to 24 hours from now
    auto expiresAt = trantor::Date::now().after(24 * 3600);
    newUser.setEmailVerificationExpires(expiresAt);

    try {
        mapper.insert(newUser);
        
        // Send verification email
        EmailHelper::sendVerificationEmail(email, username, verificationToken);
        
        Json::Value ret;
        ret["message"] = "Registration successful. Please check your email to verify your account.";
        ret["username"] = username;
        ret["email_sent"] = true;
        auto resp = HttpResponse::newHttpJsonResponse(ret);
        resp->setStatusCode(k201Created);
        callback(resp);
    } catch (const DrogonDbException &e) {
        LOG_ERROR << "DB Error: " << e.base().what();
        Json::Value ret;
        ret["error"] = "Registration failed";
        auto resp = HttpResponse::newHttpJsonResponse(ret);
        resp->setStatusCode(k500InternalServerError);
        callback(resp);
    }
}

void AuthController::loginUser(const HttpRequestPtr &req,
                               std::function<void(const HttpResponsePtr &)> &&callback)
{
    auto json = req->getJsonObject();
    if (!json) {
        auto resp = HttpResponse::newHttpJsonResponse(
            Json::Value("error: Invalid JSON"));
        resp->setStatusCode(k400BadRequest);
        callback(resp);
        return;
    }

    std::string username = (*json)["username"].asString();
    std::string password = (*json)["password"].asString();

    if (username.empty() || password.empty()) {
        Json::Value ret;
        ret["error"] = "Username and password are required";
        auto resp = HttpResponse::newHttpJsonResponse(ret);
        resp->setStatusCode(k400BadRequest);
        callback(resp);
        return;
    }

    auto dbClient = drogon::app().getDbClient();
    Mapper<drogon_model::blog_db::Users> mapper(dbClient);

    try {
        auto users = mapper.findBy(
            Criteria(drogon_model::blog_db::Users::Cols::_username, 
                    CompareOperator::EQ, username)
        );

        if (users.size() == 0) {
            Json::Value ret;
            ret["error"] = "Invalid credentials";
            auto resp = HttpResponse::newHttpJsonResponse(ret);
            resp->setStatusCode(k401Unauthorized);
            callback(resp);
            return;
        }

        auto user = users[0];

        if (!verifyPassword(user.getValueOfPasswordHash(), password)) {
            Json::Value ret;
            ret["error"] = "Invalid credentials";
            auto resp = HttpResponse::newHttpJsonResponse(ret);
            resp->setStatusCode(k401Unauthorized);
            callback(resp);
            return;
        }

        // Set session
        auto session = req->session();
        session->insert("user_id", static_cast<int>(user.getValueOfId()));
        session->insert("username", user.getValueOfUsername());

        Json::Value ret;
        ret["message"] = "Login successful";
        ret["user"]["id"] = user.getValueOfId();
        ret["user"]["username"] = user.getValueOfUsername();
        ret["user"]["email"] = user.getValueOfEmail();
        
        auto resp = HttpResponse::newHttpJsonResponse(ret);
        callback(resp);
    } catch (const DrogonDbException &e) {
        LOG_ERROR << "DB Error: " << e.base().what();
        Json::Value ret;
        ret["error"] = "Login failed";
        auto resp = HttpResponse::newHttpJsonResponse(ret);
        resp->setStatusCode(k500InternalServerError);
        callback(resp);
    }
}

void AuthController::logoutUser(const HttpRequestPtr &req,
                                std::function<void(const HttpResponsePtr &)> &&callback)
{
    auto session = req->session();
    session->erase("user_id");
    session->erase("username");

    Json::Value ret;
    ret["message"] = "Logout successful";
    auto resp = HttpResponse::newHttpJsonResponse(ret);
    callback(resp);
}

void AuthController::getCurrentUser(const HttpRequestPtr &req,
                                   std::function<void(const HttpResponsePtr &)> &&callback)
{
    auto session = req->session();
    auto userIdOpt = session->getOptional<int>("user_id");

    if (!userIdOpt.has_value()) {
        Json::Value ret;
        ret["error"] = "Not authenticated";
        auto resp = HttpResponse::newHttpJsonResponse(ret);
        resp->setStatusCode(k401Unauthorized);
        callback(resp);
        return;
    }

    auto dbClient = drogon::app().getDbClient();
    Mapper<drogon_model::blog_db::Users> mapper(dbClient);

    try {
        auto user = mapper.findByPrimaryKey(userIdOpt.value());

        Json::Value ret;
        ret["id"] = user.getValueOfId();
        ret["username"] = user.getValueOfUsername();
        ret["email"] = user.getValueOfEmail();
        ret["bio"] = user.getValueOfBio();
        
        if (!user.getValueOfProfileImage().empty()) {
            ret["profile_image"] = user.getValueOfProfileImage();
        }

        auto resp = HttpResponse::newHttpJsonResponse(ret);
        callback(resp);
    } catch (const DrogonDbException &e) {
        LOG_ERROR << "DB Error: " << e.base().what();
        Json::Value ret;
        ret["error"] = "User not found";
        auto resp = HttpResponse::newHttpJsonResponse(ret);
        resp->setStatusCode(k404NotFound);
        callback(resp);
    }
}

void AuthController::verifyEmail(const HttpRequestPtr &req,
                                 std::function<void(const HttpResponsePtr &)> &&callback)
{
    auto json = req->getJsonObject();
    if (!json) {
        auto resp = HttpResponse::newHttpJsonResponse(
            Json::Value("error: Invalid JSON"));
        resp->setStatusCode(k400BadRequest);
        callback(resp);
        return;
    }

    std::string token = (*json)["token"].asString();

    if (token.empty()) {
        Json::Value ret;
        ret["error"] = "Token is required";
        auto resp = HttpResponse::newHttpJsonResponse(ret);
        resp->setStatusCode(k400BadRequest);
        callback(resp);
        return;
    }

    auto dbClient = drogon::app().getDbClient();
    Mapper<drogon_model::blog_db::Users> mapper(dbClient);

    try {
        auto users = mapper.findBy(
            Criteria(drogon_model::blog_db::Users::Cols::_email_verification_token, 
                    CompareOperator::EQ, token)
        );

        if (users.size() == 0) {
            Json::Value ret;
            ret["error"] = "Invalid or expired token";
            auto resp = HttpResponse::newHttpJsonResponse(ret);
            resp->setStatusCode(k400BadRequest);
            callback(resp);
            return;
        }

        auto user = users[0];

        // Check if token is expired
        if (user.getValueOfEmailVerificationExpires() < trantor::Date::now()) {
            Json::Value ret;
            ret["error"] = "Token has expired. Please request a new one.";
            auto resp = HttpResponse::newHttpJsonResponse(ret);
            resp->setStatusCode(k400BadRequest);
            callback(resp);
            return;
        }

        // Verify email
        user.setEmailVerified(1);
        user.setEmailVerificationToken("");
        user.setEmailVerificationExpires(trantor::Date());
        mapper.update(user);

        Json::Value ret;
        ret["message"] = "Email verified successfully";
        auto resp = HttpResponse::newHttpJsonResponse(ret);
        callback(resp);
    } catch (const DrogonDbException &e) {
        LOG_ERROR << "DB Error: " << e.base().what();
        Json::Value ret;
        ret["error"] = "Verification failed";
        auto resp = HttpResponse::newHttpJsonResponse(ret);
        resp->setStatusCode(k500InternalServerError);
        callback(resp);
    }
}

void AuthController::requestPasswordReset(const HttpRequestPtr &req,
                                         std::function<void(const HttpResponsePtr &)> &&callback)
{
    auto json = req->getJsonObject();
    if (!json) {
        auto resp = HttpResponse::newHttpJsonResponse(
            Json::Value("error: Invalid JSON"));
        resp->setStatusCode(k400BadRequest);
        callback(resp);
        return;
    }

    std::string email = (*json)["email"].asString();

    if (email.empty()) {
        Json::Value ret;
        ret["error"] = "Email is required";
        auto resp = HttpResponse::newHttpJsonResponse(ret);
        resp->setStatusCode(k400BadRequest);
        callback(resp);
        return;
    }

    auto dbClient = drogon::app().getDbClient();
    Mapper<drogon_model::blog_db::Users> userMapper(dbClient);
    Mapper<drogon_model::blog_db::PasswordResetTokens> tokenMapper(dbClient);

    try {
        auto users = userMapper.findBy(
            Criteria(drogon_model::blog_db::Users::Cols::_email, 
                    CompareOperator::EQ, email)
        );

        if (users.size() == 0) {
            // Don't reveal if email exists or not for security
            Json::Value ret;
            ret["message"] = "If an account exists with that email, a password reset link has been sent.";
            auto resp = HttpResponse::newHttpJsonResponse(ret);
            callback(resp);
            return;
        }

        auto user = users[0];
        
        // Generate reset token
        std::string resetToken = EmailHelper::generateToken();
        
        // Create reset token record
        drogon_model::blog_db::PasswordResetTokens resetRecord;
        resetRecord.setUserId(user.getValueOfId());
        resetRecord.setToken(resetToken);
        resetRecord.setExpiresAt(trantor::Date::now().after(3600)); // 1 hour expiry
        
        tokenMapper.insert(resetRecord);
        
        // Send reset email
        EmailHelper::sendPasswordResetEmail(email, user.getValueOfUsername(), resetToken);

        Json::Value ret;
        ret["message"] = "If an account exists with that email, a password reset link has been sent.";
        auto resp = HttpResponse::newHttpJsonResponse(ret);
        callback(resp);
    } catch (const DrogonDbException &e) {
        LOG_ERROR << "DB Error: " << e.base().what();
        Json::Value ret;
        ret["error"] = "Failed to process request";
        auto resp = HttpResponse::newHttpJsonResponse(ret);
        resp->setStatusCode(k500InternalServerError);
        callback(resp);
    }
}

void AuthController::resetPassword(const HttpRequestPtr &req,
                                  std::function<void(const HttpResponsePtr &)> &&callback)
{
    auto json = req->getJsonObject();
    if (!json) {
        auto resp = HttpResponse::newHttpJsonResponse(
            Json::Value("error: Invalid JSON"));
        resp->setStatusCode(k400BadRequest);
        callback(resp);
        return;
    }

    std::string token = (*json)["token"].asString();
    std::string newPassword = (*json)["password"].asString();

    if (token.empty() || newPassword.empty()) {
        Json::Value ret;
        ret["error"] = "Token and password are required";
        auto resp = HttpResponse::newHttpJsonResponse(ret);
        resp->setStatusCode(k400BadRequest);
        callback(resp);
        return;
    }

    auto dbClient = drogon::app().getDbClient();
    Mapper<drogon_model::blog_db::PasswordResetTokens> tokenMapper(dbClient);
    Mapper<drogon_model::blog_db::Users> userMapper(dbClient);

    try {
        auto tokens = tokenMapper.findBy(
            Criteria(drogon_model::blog_db::PasswordResetTokens::Cols::_token, 
                    CompareOperator::EQ, token)
        );

        if (tokens.size() == 0) {
            Json::Value ret;
            ret["error"] = "Invalid or expired token";
            auto resp = HttpResponse::newHttpJsonResponse(ret);
            resp->setStatusCode(k400BadRequest);
            callback(resp);
            return;
        }

        auto resetToken = tokens[0];

        // Check if token is expired
        if (resetToken.getValueOfExpiresAt() < trantor::Date::now()) {
            // Delete expired token
            tokenMapper.deleteByPrimaryKey(resetToken.getValueOfId());
            
            Json::Value ret;
            ret["error"] = "Token has expired. Please request a new one.";
            auto resp = HttpResponse::newHttpJsonResponse(ret);
            resp->setStatusCode(k400BadRequest);
            callback(resp);
            return;
        }

        // Get user and update password
        auto user = userMapper.findByPrimaryKey(resetToken.getValueOfUserId());
        user.setPasswordHash(hashPassword(newPassword));
        userMapper.update(user);

        // Delete used token
        tokenMapper.deleteByPrimaryKey(resetToken.getValueOfId());

        Json::Value ret;
        ret["message"] = "Password reset successfully";
        auto resp = HttpResponse::newHttpJsonResponse(ret);
        callback(resp);
    } catch (const DrogonDbException &e) {
        LOG_ERROR << "DB Error: " << e.base().what();
        Json::Value ret;
        ret["error"] = "Failed to reset password";
        auto resp = HttpResponse::newHttpJsonResponse(ret);
        resp->setStatusCode(k500InternalServerError);
        callback(resp);
    }
}

void AuthController::resendVerification(const HttpRequestPtr &req,
                                       std::function<void(const HttpResponsePtr &)> &&callback)
{
    auto json = req->getJsonObject();
    if (!json) {
        auto resp = HttpResponse::newHttpJsonResponse(
            Json::Value("error: Invalid JSON"));
        resp->setStatusCode(k400BadRequest);
        callback(resp);
        return;
    }

    std::string email = (*json)["email"].asString();

    if (email.empty()) {
        Json::Value ret;
        ret["error"] = "Email is required";
        auto resp = HttpResponse::newHttpJsonResponse(ret);
        resp->setStatusCode(k400BadRequest);
        callback(resp);
        return;
    }

    auto dbClient = drogon::app().getDbClient();
    Mapper<drogon_model::blog_db::Users> mapper(dbClient);

    try {
        auto users = mapper.findBy(
            Criteria(drogon_model::blog_db::Users::Cols::_email, 
                    CompareOperator::EQ, email)
        );

        if (users.size() == 0) {
            Json::Value ret;
            ret["error"] = "User not found";
            auto resp = HttpResponse::newHttpJsonResponse(ret);
            resp->setStatusCode(k404NotFound);
            callback(resp);
            return;
        }

        auto user = users[0];

        if (user.getValueOfEmailVerified() == 1) {
            Json::Value ret;
            ret["error"] = "Email is already verified";
            auto resp = HttpResponse::newHttpJsonResponse(ret);
            resp->setStatusCode(k400BadRequest);
            callback(resp);
            return;
        }

        // Generate new verification token
        std::string verificationToken = EmailHelper::generateToken();
        user.setEmailVerificationToken(verificationToken);
        user.setEmailVerificationExpires(trantor::Date::now().after(24 * 3600));
        
        mapper.update(user);

        // Send verification email
        EmailHelper::sendVerificationEmail(email, user.getValueOfUsername(), verificationToken);

        Json::Value ret;
        ret["message"] = "Verification email sent";
        auto resp = HttpResponse::newHttpJsonResponse(ret);
        callback(resp);
    } catch (const DrogonDbException &e) {
        LOG_ERROR << "DB Error: " << e.base().what();
        Json::Value ret;
        ret["error"] = "Failed to resend verification";
        auto resp = HttpResponse::newHttpJsonResponse(ret);
        resp->setStatusCode(k500InternalServerError);
        callback(resp);
    }
}
