#pragma once

#include <drogon/HttpController.h>

using namespace drogon;

class UserController : public drogon::HttpController<UserController>
{
  public:
    METHOD_LIST_BEGIN
    // Get user profile
    ADD_METHOD_TO(UserController::getUserProfile, "/users/{1}", Get);
    // Update user profile
    ADD_METHOD_TO(UserController::updateProfile, "/users/profile", Put);
    // Upload profile image
    ADD_METHOD_TO(UserController::uploadProfileImage, "/users/profile/image", Post);
    // Get all users (for messaging)
    ADD_METHOD_TO(UserController::getAllUsers, "/users", Get);
    METHOD_LIST_END
    
    void getUserProfile(const HttpRequestPtr &req,
                       std::function<void(const HttpResponsePtr &)> &&callback,
                       int userId);
    void updateProfile(const HttpRequestPtr &req,
                      std::function<void(const HttpResponsePtr &)> &&callback);
    void uploadProfileImage(const HttpRequestPtr &req,
                           std::function<void(const HttpResponsePtr &)> &&callback);
    void getAllUsers(const HttpRequestPtr &req,
                    std::function<void(const HttpResponsePtr &)> &&callback);
};
