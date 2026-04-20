#pragma once

#include <drogon/HttpController.h>

using namespace drogon;

class PostController : public drogon::HttpController<PostController>
{
  public:
    METHOD_LIST_BEGIN
    // Get all posts
    ADD_METHOD_TO(PostController::getAllPosts, "/posts", Get);
    // Get single post
    ADD_METHOD_TO(PostController::getPost, "/posts/{1}", Get);
    // Create post
    ADD_METHOD_TO(PostController::createPost, "/posts", Post);
    // Update post
    ADD_METHOD_TO(PostController::updatePost, "/posts/{1}", Put);
    // Delete post
    ADD_METHOD_TO(PostController::deletePost, "/posts/{1}", Delete);
    // Get user's posts
    ADD_METHOD_TO(PostController::getUserPosts, "/posts/user/{1}", Get);
    // Like post
    ADD_METHOD_TO(PostController::likePost, "/posts/{1}/like", Post);
    // Unlike post
    ADD_METHOD_TO(PostController::unlikePost, "/posts/{1}/like", Delete);
    // Get post likes count
    ADD_METHOD_TO(PostController::getLikesCount, "/posts/{1}/likes", Get);
    METHOD_LIST_END
    
    void getAllPosts(const HttpRequestPtr &req,
                    std::function<void(const HttpResponsePtr &)> &&callback);
    void getPost(const HttpRequestPtr &req,
                std::function<void(const HttpResponsePtr &)> &&callback,
                int postId);
    void createPost(const HttpRequestPtr &req,
                   std::function<void(const HttpResponsePtr &)> &&callback);
    void updatePost(const HttpRequestPtr &req,
                   std::function<void(const HttpResponsePtr &)> &&callback,
                   int postId);
    void deletePost(const HttpRequestPtr &req,
                   std::function<void(const HttpResponsePtr &)> &&callback,
                   int postId);
    void getUserPosts(const HttpRequestPtr &req,
                     std::function<void(const HttpResponsePtr &)> &&callback,
                     int userId);
    void likePost(const HttpRequestPtr &req,
                 std::function<void(const HttpResponsePtr &)> &&callback,
                 int postId);
    void unlikePost(const HttpRequestPtr &req,
                   std::function<void(const HttpResponsePtr &)> &&callback,
                   int postId);
    void getLikesCount(const HttpRequestPtr &req,
                      std::function<void(const HttpResponsePtr &)> &&callback,
                      int postId);
};
