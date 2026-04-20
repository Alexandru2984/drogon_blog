#pragma once

#include <drogon/HttpController.h>

using namespace drogon;

class AuthController : public drogon::HttpController<AuthController>
{
  public:
    METHOD_LIST_BEGIN
    // Register
    ADD_METHOD_TO(AuthController::registerUser, "/auth/register", Post);
    // Login
    ADD_METHOD_TO(AuthController::loginUser, "/auth/login", Post);
    // Logout
    ADD_METHOD_TO(AuthController::logoutUser, "/auth/logout", Post);
    // Get current user
    ADD_METHOD_TO(AuthController::getCurrentUser, "/auth/me", Get);
    // Verify email
    ADD_METHOD_TO(AuthController::verifyEmail, "/auth/verify-email", Post);
    // Request password reset
    ADD_METHOD_TO(AuthController::requestPasswordReset, "/auth/request-reset", Post);
    // Reset password
    ADD_METHOD_TO(AuthController::resetPassword, "/auth/reset-password", Post);
    // Resend verification email
    ADD_METHOD_TO(AuthController::resendVerification, "/auth/resend-verification", Post);
    METHOD_LIST_END
    
    void registerUser(const HttpRequestPtr &req,
                     std::function<void(const HttpResponsePtr &)> &&callback);
    void loginUser(const HttpRequestPtr &req,
                  std::function<void(const HttpResponsePtr &)> &&callback);
    void logoutUser(const HttpRequestPtr &req,
                   std::function<void(const HttpResponsePtr &)> &&callback);
    void getCurrentUser(const HttpRequestPtr &req,
                       std::function<void(const HttpResponsePtr &)> &&callback);
    void verifyEmail(const HttpRequestPtr &req,
                    std::function<void(const HttpResponsePtr &)> &&callback);
    void requestPasswordReset(const HttpRequestPtr &req,
                             std::function<void(const HttpResponsePtr &)> &&callback);
    void resetPassword(const HttpRequestPtr &req,
                      std::function<void(const HttpResponsePtr &)> &&callback);
    void resendVerification(const HttpRequestPtr &req,
                           std::function<void(const HttpResponsePtr &)> &&callback);
};
