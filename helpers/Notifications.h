#pragma once

#include <drogon/orm/DbClient.h>
#include <json/json.h>

#include <string>

// In-app notifications.
//
// Every emit here is best-effort and never propagates a failure to the
// action that caused it. A comment that saved but failed to notify is a
// missing notification; a comment that refused to save because notifying
// failed is lost writing. The former is recoverable by reloading the page,
// the latter is not.
namespace notifications {

enum class Kind {
    Comment,   // someone commented on your post
    Reply,     // someone replied to your comment
    Follow,    // someone followed you
    NewPost,   // an author you follow published something
    Like,      // someone liked your post
};

const char* kindName(Kind k);

// Create one notification. Silently does nothing when recipient == actor:
// nobody needs telling about their own actions, and filtering here means no
// call site has to remember to.
void emit(const drogon::orm::DbClientPtr& db,
          int recipientUserId,
          int actorUserId,
          Kind kind,
          int postId = 0,        // 0 = not about a post
          int commentId = 0);    // 0 = not about a comment

// Fan a new post out to everyone following the author. One INSERT ... SELECT
// rather than a loop: an author with ten thousand followers would otherwise
// be ten thousand round trips inside a request handler.
void emitNewPostToFollowers(const drogon::orm::DbClientPtr& db,
                            int authorUserId,
                            int postId);

// The notification list for one user, newest first, with the actor and the
// target resolved so the client can render a line without further calls.
Json::Value list(const drogon::orm::DbClientPtr& db,
                 int userId,
                 int limit = 50,
                 long long beforeId = 0);

// How many are unread. Drives the navigation badge, so it is called on every
// page load and reads from a partial index rather than counting the history.
long long unreadCount(const drogon::orm::DbClientPtr& db, int userId);

// Mark one notification read. Scoped to the owner in the WHERE clause rather
// than checked first and updated after, so there is no window in which
// another user's row could be marked.
bool markRead(const drogon::orm::DbClientPtr& db, int userId, long long id);

// Mark everything read. Returns how many rows changed, so the UI can say
// something specific rather than just clearing the badge.
long long markAllRead(const drogon::orm::DbClientPtr& db, int userId);

} // namespace notifications
