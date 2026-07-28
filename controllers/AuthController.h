#pragma once

#include <drogon/HttpController.h>

using namespace drogon;

class AuthController : public drogon::HttpController<AuthController>
{
  public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(AuthController::registerUser,           "/auth/register",            Post);
    ADD_METHOD_TO(AuthController::loginUser,              "/auth/login",               Post);
    ADD_METHOD_TO(AuthController::logoutUser,             "/auth/logout",              Post);
    ADD_METHOD_TO(AuthController::getCurrentUser,         "/auth/me",                  Get);
    ADD_METHOD_TO(AuthController::verifyEmail,            "/auth/verify-email",        Post);
    ADD_METHOD_TO(AuthController::requestPasswordReset,   "/auth/request-reset",       Post);
    ADD_METHOD_TO(AuthController::resetPassword,          "/auth/reset-password",      Post);
    ADD_METHOD_TO(AuthController::resendVerification,     "/auth/resend-verification", Post);

    // ---- 2FA enrolment + management (authenticated user) ----
    ADD_METHOD_TO(AuthController::status2fa,              "/auth/2fa/status",                       Get);
    ADD_METHOD_TO(AuthController::setupTotp,              "/auth/2fa/totp/setup",                   Post);
    ADD_METHOD_TO(AuthController::confirmTotp,            "/auth/2fa/totp/confirm",                 Post);
    ADD_METHOD_TO(AuthController::disable2fa,             "/auth/2fa/disable",                      Post);
    ADD_METHOD_TO(AuthController::regenerateRecoveryCodes,"/auth/2fa/recovery-codes/regenerate",    Post);

    ADD_METHOD_TO(AuthController::webauthnRegisterBegin,  "/auth/2fa/webauthn/register/begin",      Post);
    ADD_METHOD_TO(AuthController::webauthnRegisterFinish, "/auth/2fa/webauthn/register/finish",     Post);
    ADD_METHOD_TO(AuthController::webauthnList,           "/auth/2fa/webauthn/list",                Get);
    ADD_METHOD_TO(AuthController::webauthnRemove,         "/auth/2fa/webauthn/remove/{1}",          Post);

    // ---- Account security (authenticated user) ----
    ADD_METHOD_TO(AuthController::changePassword,         "/auth/change-password",                  Post);
    ADD_METHOD_TO(AuthController::listSessions,           "/auth/sessions",                         Get);
    ADD_METHOD_TO(AuthController::revokeSession,          "/auth/sessions/revoke",                  Post);
    ADD_METHOD_TO(AuthController::revokeOtherSessions,    "/auth/sessions/revoke-others",           Post);

    // ---- Two-step login completion (unauthenticated, session-bound) ----
    ADD_METHOD_TO(AuthController::verifyLoginTotp,        "/auth/login/verify-totp",                Post);
    ADD_METHOD_TO(AuthController::verifyLoginRecovery,    "/auth/login/verify-recovery",            Post);
    ADD_METHOD_TO(AuthController::webauthnLoginBegin,     "/auth/login/verify-webauthn/begin",      Post);
    ADD_METHOD_TO(AuthController::webauthnLoginFinish,    "/auth/login/verify-webauthn/finish",     Post);
    METHOD_LIST_END

    // Account security. Implementations live in AuthControllerAccount.cc.
    void changePassword(const HttpRequestPtr &req,
                       std::function<void(const HttpResponsePtr &)> &&callback);
    void listSessions(const HttpRequestPtr &req,
                     std::function<void(const HttpResponsePtr &)> &&callback);
    void revokeSession(const HttpRequestPtr &req,
                      std::function<void(const HttpResponsePtr &)> &&callback);
    void revokeOtherSessions(const HttpRequestPtr &req,
                            std::function<void(const HttpResponsePtr &)> &&callback);

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

    // 2FA management
    void status2fa(const HttpRequestPtr&,
                   std::function<void(const HttpResponsePtr&)>&&);
    void setupTotp(const HttpRequestPtr&,
                   std::function<void(const HttpResponsePtr&)>&&);
    void confirmTotp(const HttpRequestPtr&,
                     std::function<void(const HttpResponsePtr&)>&&);
    void disable2fa(const HttpRequestPtr&,
                    std::function<void(const HttpResponsePtr&)>&&);
    void regenerateRecoveryCodes(const HttpRequestPtr&,
                                 std::function<void(const HttpResponsePtr&)>&&);
    void webauthnRegisterBegin(const HttpRequestPtr&,
                               std::function<void(const HttpResponsePtr&)>&&);
    void webauthnRegisterFinish(const HttpRequestPtr&,
                                std::function<void(const HttpResponsePtr&)>&&);
    void webauthnList(const HttpRequestPtr&,
                      std::function<void(const HttpResponsePtr&)>&&);
    void webauthnRemove(const HttpRequestPtr&,
                        std::function<void(const HttpResponsePtr&)>&&,
                        std::int64_t credentialId);

    // Two-step login completion
    void verifyLoginTotp(const HttpRequestPtr&,
                         std::function<void(const HttpResponsePtr&)>&&);
    void verifyLoginRecovery(const HttpRequestPtr&,
                             std::function<void(const HttpResponsePtr&)>&&);
    void webauthnLoginBegin(const HttpRequestPtr&,
                            std::function<void(const HttpResponsePtr&)>&&);
    void webauthnLoginFinish(const HttpRequestPtr&,
                             std::function<void(const HttpResponsePtr&)>&&);
};
