#pragma once

#include <drogon/HttpController.h>

using namespace drogon;

// Abuse reporting (any signed-in user) and the moderation queue
// (moderator and above).
//
// The privileged routes live under /admin deliberately: it keeps them
// obvious in the router, in the access log and in nginx, so an operator
// can reason about — or block — the whole surface as one prefix.
class ModerationController : public drogon::HttpController<ModerationController>
{
  public:
    METHOD_LIST_BEGIN
    // Any authenticated user.
    ADD_METHOD_TO(ModerationController::createReport,   "/reports",                    Post);

    // Moderator and above. These answer 404 rather than 403 to a caller
    // without the role — see helpers/Roles.h for why.
    ADD_METHOD_TO(ModerationController::listReports,    "/admin/reports",              Get);
    ADD_METHOD_TO(ModerationController::resolveReport,  "/admin/reports/{1}/resolve",  Post);
    ADD_METHOD_TO(ModerationController::hidePost,       "/admin/posts/{1}/hide",       Post);
    ADD_METHOD_TO(ModerationController::unhidePost,     "/admin/posts/{1}/unhide",     Post);
    ADD_METHOD_TO(ModerationController::hideComment,    "/admin/comments/{1}/hide",    Post);
    ADD_METHOD_TO(ModerationController::unhideComment,  "/admin/comments/{1}/unhide",  Post);
    ADD_METHOD_TO(ModerationController::banUser,        "/admin/users/{1}/ban",        Post);
    ADD_METHOD_TO(ModerationController::unbanUser,      "/admin/users/{1}/unban",      Post);
    METHOD_LIST_END

    void createReport(const HttpRequestPtr& req,
                     std::function<void(const HttpResponsePtr&)>&& callback);
    void listReports(const HttpRequestPtr& req,
                    std::function<void(const HttpResponsePtr&)>&& callback);
    void resolveReport(const HttpRequestPtr& req,
                      std::function<void(const HttpResponsePtr&)>&& callback,
                      int reportId);
    void hidePost(const HttpRequestPtr& req,
                 std::function<void(const HttpResponsePtr&)>&& callback,
                 int postId);
    void unhidePost(const HttpRequestPtr& req,
                   std::function<void(const HttpResponsePtr&)>&& callback,
                   int postId);
    void hideComment(const HttpRequestPtr& req,
                    std::function<void(const HttpResponsePtr&)>&& callback,
                    int commentId);
    void unhideComment(const HttpRequestPtr& req,
                      std::function<void(const HttpResponsePtr&)>&& callback,
                      int commentId);
    void banUser(const HttpRequestPtr& req,
                std::function<void(const HttpResponsePtr&)>&& callback,
                int userId);
    void unbanUser(const HttpRequestPtr& req,
                  std::function<void(const HttpResponsePtr&)>&& callback,
                  int userId);
};
