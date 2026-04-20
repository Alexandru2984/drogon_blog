#pragma once

#include <drogon/HttpController.h>

using namespace drogon;

class CommentController : public drogon::HttpController<CommentController>
{
  public:
    METHOD_LIST_BEGIN
    // Get comments for a post
    ADD_METHOD_TO(CommentController::getPostComments, "/posts/{1}/comments", Get);
    // Create comment
    ADD_METHOD_TO(CommentController::createComment, "/posts/{1}/comments", Post);
    // Update comment
    ADD_METHOD_TO(CommentController::updateComment, "/comments/{1}", Put);
    // Delete comment
    ADD_METHOD_TO(CommentController::deleteComment, "/comments/{1}", Delete);
    METHOD_LIST_END
    
    void getPostComments(const HttpRequestPtr &req,
                        std::function<void(const HttpResponsePtr &)> &&callback,
                        int postId);
    void createComment(const HttpRequestPtr &req,
                      std::function<void(const HttpResponsePtr &)> &&callback,
                      int postId);
    void updateComment(const HttpRequestPtr &req,
                      std::function<void(const HttpResponsePtr &)> &&callback,
                      int commentId);
    void deleteComment(const HttpRequestPtr &req,
                      std::function<void(const HttpResponsePtr &)> &&callback,
                      int commentId);
};
