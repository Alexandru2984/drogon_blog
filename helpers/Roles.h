#pragma once

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>

#include <optional>
#include <string>

// Roles and account standing.
//
// Until now every account was equal, so abusive content could only be
// dealt with by editing the database by hand. These helpers are the shared
// gate for the moderation endpoints — one place that decides what a caller
// is allowed to do, rather than each handler reimplementing the check and
// one of them getting it wrong.
namespace roles {

enum class Role { User, Moderator, Admin };

// Ordering is what the checks rely on: Admin satisfies a Moderator
// requirement, never the reverse.
bool atLeast(Role have, Role need);

Role        parse(const std::string& s);
const char* name(Role r);

// The caller's role, or nullopt when not authenticated. Blocking — reads
// the users row, so call from a worker thread.
std::optional<Role> of(const drogon::HttpRequestPtr& req);

// Returns nullptr when the caller holds `need` or better; otherwise a
// ready-to-send response.
//
// 404 rather than 403 for an authenticated caller who lacks the role. The
// moderation surface is not something an ordinary user should be able to
// map: a 403 confirms the endpoint exists and is worth attacking, while a
// 404 makes probing it indistinguishable from probing nonsense. A missing
// session still gets 401, since that is not a secret.
//
// Blocking — call from a worker thread.
drogon::HttpResponsePtr require(const drogon::HttpRequestPtr& req, Role need);

// Whether the account is currently banned, and until when. Checked on
// login and before any content mutation, because a ban that only blocks
// new logins leaves an already-signed-in abuser working.
struct BanState {
    bool        banned = false;
    std::string until;      // ISO-ish timestamp, empty when permanent
    std::string reason;
};
BanState banStateOf(int userId);

// Guard for content-mutating handlers. Returns nullptr when the user may
// write. Blocking.
drogon::HttpResponsePtr blockIfBanned(int userId);

// Loads the banned set and installs the advice that refuses writes from a
// suspended account. Call once, after loadConfigJson and before run().
//
// Enforced centrally rather than in each handler: there are fifteen
// mutating endpoints today and any new one would silently be exempt if
// every handler had to remember its own check. A ban that still lets
// someone post is not a ban.
void install();

// True when the user is currently suspended, read from the in-memory set.
// Cheap — no database access.
bool isBanned(int userId);

// Applies a ban change observed on the blog_event channel.
void onBanChangedNotification(int userId, bool banned);

// True when the account has been erased (see migrations/0015). The row
// still exists — it has to, because tombstoned comments reference it — but
// it carries no personal data and every read path should treat it as
// absent.
//
// Read straight from the database rather than from an in-memory set like
// isBanned: erasure is a once-per-account event, the endpoints that ask are
// not hot, and a stale cache here would mean showing a profile that was
// deleted.
//
// Blocking — call from a worker thread or an already-blocking path.
bool isErased(int userId);

} // namespace roles
