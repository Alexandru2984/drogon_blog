#pragma once

#include <drogon/HttpRequest.h>
#include <json/json.h>

#include <functional>
#include <optional>
#include <string>

// Session registry: lets a user see where their account is signed in and
// revoke anything they do not recognise.
//
// Drogon owns the sessions (in-memory, keyed by the session cookie) and
// exposes no way to enumerate or invalidate one from outside the request
// that holds it. So a stolen session could not be dealt with at all — no
// "sign out everywhere", and a password change left an attacker's live
// session untouched, which is the one moment a user most expects it to be
// cut off.
//
// The registry shadows Drogon's store. At login we mint a random `sid`,
// keep it inside the session payload, and record a row for it. The sid is
// not a credential: it identifies a session but does not authenticate one,
// which still requires the cookie.
//
// Revocation is enforced by a sync advice that runs before every handler
// and clears the session when its sid has been revoked. Checking the
// database per request would be a query on every authenticated hit, so each
// process keeps an in-memory set of revoked sids instead and the check
// costs a hash lookup. The set is fed by the `session_revoked` event on the
// existing blog_event channel, so a revocation issued by one process
// reaches all of them.
namespace sessions {

// Registers the revocation advice and reconciles the table with reality.
// Call once, after loadConfigJson and before run().
//
// Reconciliation matters: Drogon's session store is in-memory, so a restart
// invalidates every session that existed. Without marking those rows
// revoked, the session list would keep advertising sessions that cannot be
// used and users would "revoke" ghosts.
void install();

// Mints a sid for a freshly authenticated session, records it, and returns
// it. Call after session->changeSessionIdToClient() on every path that
// completes a login (password-only and two-step alike).
std::string begin(const drogon::HttpRequestPtr& req, int userId);

// The sid of the request's session, if it has one.
std::optional<std::string> currentSid(const drogon::HttpRequestPtr& req);

// Marks one session revoked. `userId` scopes the update so a caller cannot
// revoke a session that is not theirs even if they guess a sid. Returns
// false when nothing matched. Safe to call from a worker thread.
bool revoke(int userId, const std::string& sid, const std::string& reason);

// Revokes every live session for the user except `keepSid` (pass an empty
// string to revoke all of them). Returns how many were revoked.
int revokeOthers(int userId, const std::string& keepSid,
                 const std::string& reason);

// Live sessions for the user, newest activity first, as a JSON array.
// Each entry carries sid, created_at, last_seen_at, ip, user_agent and
// `current` for the session making the request.
Json::Value list(int userId, const std::string& currentSid);

// Applies a revocation observed on the blog_event channel. Called from the
// pg_notify listener in main().
void onRevokedNotification(const std::string& sid);

// Installs a callback invoked with the sid every time a session becomes
// revoked in this process — whether the revocation was issued here or
// arrived on blog_event from another one.
//
// The hook exists because the revocation advice only fires on the *next*
// HTTP request of a session, which is the wrong moment for anything
// already holding an open connection. main() points this at
// MessageWebSocket::closeForSession so a hung-up session's live socket
// stops receiving that user's private messages immediately.
//
// Kept as a callback rather than a direct call so this helper does not
// have to know that a WebSocket layer exists. Call once, before run();
// the observer runs on whichever thread performed the revocation.
void setRevocationObserver(std::function<void(const std::string&)> observer);

} // namespace sessions
