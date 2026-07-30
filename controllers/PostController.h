#pragma once

#include <drogon/HttpController.h>

using namespace drogon;

class PostController : public drogon::HttpController<PostController>
{
  public:
    METHOD_LIST_BEGIN
    // Get all posts
    ADD_METHOD_TO(PostController::getAllPosts, "/posts", Get);
    // Full-text search across title + content (must be registered before the
    // /posts/{id} pattern so "/posts/search" doesn't get routed to getPost).
    ADD_METHOD_TO(PostController::searchPosts, "/posts/search", Get);
    // The signed-in author's own unpublished posts. Must also precede
    // /posts/{id} — "drafts" would otherwise be captured as an id.
    // Not /posts/user/{id} with a flag: drafts are never someone else's to
    // list, so the route has no user parameter to get wrong.
    ADD_METHOD_TO(PostController::getMyDrafts, "/posts/drafts", Get);
    // Render markdown without saving anything, so the editor's preview is
    // byte-for-byte what publishing would produce. A client-side renderer
    // would be faster but would drift from cmark-gfm, and a preview that
    // lies about the result is worse than one that costs a round trip.
    // Literal segment, so it precedes /posts/{id} too.
    ADD_METHOD_TO(PostController::previewMarkdown, "/posts/preview", Post);
    // Get single post
    ADD_METHOD_TO(PostController::getPost, "/posts/{1}", Get);
    // Upload an inline image for use in a post body (returns a URL the client
    // embeds as Markdown). Registered before the create route for clarity;
    // distinct path so it never collides with /posts or /posts/{id}.
    ADD_METHOD_TO(PostController::uploadPostImage, "/posts/images", Post);
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
    // Every tag in use, with post counts. Registered before /posts/{1} for
    // the same reason /posts/search is: a literal path segment must not be
    // parsed as an id.
    ADD_METHOD_TO(PostController::listTags, "/tags", Get);
    // Posts carrying one tag.
    ADD_METHOD_TO(PostController::getPostsByTag, "/tags/{1}/posts", Get);
    // Other posts sharing this one's tags.
    ADD_METHOD_TO(PostController::getRelatedPosts, "/posts/{1}/related", Get);
    METHOD_LIST_END
    
    void getAllPosts(const HttpRequestPtr &req,
                    std::function<void(const HttpResponsePtr &)> &&callback);
    void listTags(const HttpRequestPtr &req,
                  std::function<void(const HttpResponsePtr &)> &&callback);
    void getPostsByTag(const HttpRequestPtr &req,
                       std::function<void(const HttpResponsePtr &)> &&callback,
                       const std::string &slug);
    void getMyDrafts(const HttpRequestPtr &req,
                     std::function<void(const HttpResponsePtr &)> &&callback);
    void previewMarkdown(const HttpRequestPtr &req,
                         std::function<void(const HttpResponsePtr &)> &&callback);
    void getRelatedPosts(const HttpRequestPtr &req,
                         std::function<void(const HttpResponsePtr &)> &&callback,
                         int postId);
    void searchPosts(const HttpRequestPtr &req,
                     std::function<void(const HttpResponsePtr &)> &&callback);
    void getPost(const HttpRequestPtr &req,
                std::function<void(const HttpResponsePtr &)> &&callback,
                int postId);
    void createPost(const HttpRequestPtr &req,
                   std::function<void(const HttpResponsePtr &)> &&callback);
    void uploadPostImage(const HttpRequestPtr &req,
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
