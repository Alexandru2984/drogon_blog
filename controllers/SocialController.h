#pragma once

#include <drogon/HttpController.h>

using namespace drogon;

// Bookmarks, follows and the notification inbox.
//
// Grouped in one controller because all three are "the reader's own
// relationship to other things" — every endpoint here is scoped to the
// signed-in user and none of them is meaningful anonymously.
class SocialController : public drogon::HttpController<SocialController>
{
  public:
    METHOD_LIST_BEGIN
    // ---- bookmarks ----
    // The reader's own saved posts. A literal path segment, so it is
    // registered before any /{id} pattern that could capture it.
    ADD_METHOD_TO(SocialController::listBookmarks,  "/bookmarks",            Get);
    ADD_METHOD_TO(SocialController::addBookmark,    "/posts/{1}/bookmark",   Post);
    ADD_METHOD_TO(SocialController::removeBookmark, "/posts/{1}/bookmark",   Delete);

    // ---- follows ----
    ADD_METHOD_TO(SocialController::follow,         "/users/{1}/follow",     Post);
    ADD_METHOD_TO(SocialController::unfollow,       "/users/{1}/follow",     Delete);
    // Public: how many followers an author has is on their profile.
    ADD_METHOD_TO(SocialController::followStats,    "/users/{1}/follow-stats", Get);
    // Posts by the people the signed-in reader follows.
    ADD_METHOD_TO(SocialController::followingFeed,  "/feed/following",       Get);

    // ---- notifications ----
    ADD_METHOD_TO(SocialController::listNotifications,   "/notifications",           Get);
    ADD_METHOD_TO(SocialController::unreadCount,         "/notifications/unread",    Get);
    ADD_METHOD_TO(SocialController::markNotificationRead,"/notifications/{1}/read",  Post);
    ADD_METHOD_TO(SocialController::markAllRead,         "/notifications/read-all",  Post);
    METHOD_LIST_END

    void listBookmarks (const HttpRequestPtr &req,
                        std::function<void(const HttpResponsePtr &)> &&callback);
    void addBookmark   (const HttpRequestPtr &req,
                        std::function<void(const HttpResponsePtr &)> &&callback, int postId);
    void removeBookmark(const HttpRequestPtr &req,
                        std::function<void(const HttpResponsePtr &)> &&callback, int postId);

    void follow      (const HttpRequestPtr &req,
                      std::function<void(const HttpResponsePtr &)> &&callback, int userId);
    void unfollow    (const HttpRequestPtr &req,
                      std::function<void(const HttpResponsePtr &)> &&callback, int userId);
    void followStats (const HttpRequestPtr &req,
                      std::function<void(const HttpResponsePtr &)> &&callback, int userId);
    void followingFeed(const HttpRequestPtr &req,
                       std::function<void(const HttpResponsePtr &)> &&callback);

    void listNotifications   (const HttpRequestPtr &req,
                              std::function<void(const HttpResponsePtr &)> &&callback);
    void unreadCount         (const HttpRequestPtr &req,
                              std::function<void(const HttpResponsePtr &)> &&callback);
    void markNotificationRead(const HttpRequestPtr &req,
                              std::function<void(const HttpResponsePtr &)> &&callback,
                              const std::string &id);
    void markAllRead         (const HttpRequestPtr &req,
                              std::function<void(const HttpResponsePtr &)> &&callback);
};
