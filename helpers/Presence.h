#pragma once

#include <string>

// Cross-pod online-presence tracker backed by Redis.
//
// Why this exists: the WebSocket hub fan-out for /messages already
// scales across pods via Postgres pg_notify (PgListener subscribes
// on every pod, so an INSERT triggers fan-out everywhere). What
// pg_notify can't answer is "is user N online RIGHT NOW, on any
// pod?" — that requires shared state.
//
// Implementation: each pod marks its connected users online via
// SETEX user:<id>:online <ttl> "1". A background thread refreshes
// the TTL every (ttl/2) seconds for users still connected locally.
// When the last connection for a user drops, the pod DELs the key
// — but the TTL is a safety net in case the pod crashes before it
// can clean up. /users/{id} reads the key with EXISTS.
//
// Activation: set BLOG_REDIS_URL=redis://host:6379 (or unix://path).
// When unset, every API call is a cheap no-op and the rest of the
// app runs single-pod just as before. When the binary is built
// without libhiredis (the default on host builds; the Docker image
// has it), the helper bodies compile to no-ops regardless of env
// — see CMakeLists.txt for the BLOG_HAS_REDIS flag.
namespace presence {

// Connect to the URL in BLOG_REDIS_URL and start the heartbeat
// thread. Idempotent — safe to call once at startup. Returns true
// if Redis is now connected; false if BLOG_REDIS_URL was empty,
// the build lacks hiredis, or the initial CONNECT failed (logged).
bool install();

// Mark a user online on this pod. Adds them to the locally-tracked
// set so the heartbeat refreshes their TTL. Cheap no-op when not
// connected.
void markOnline(int userId);

// Local connection for this user closed. If this was the last one,
// pop the Redis key. Cheap no-op when not connected.
void markOffline(int userId);

// EXISTS check on user:<id>:online. Returns false when not connected,
// or when the key is gone (user offline / TTL expired). Synchronous
// network round-trip; cache callers should treat this as a cheap
// HTTP-handler-rate operation.
bool isOnline(int userId);

// Total number of online users across the cluster (`SCARD` on a
// global set, kept in sync via the same heartbeat). Returns -1 when
// Redis isn't available — callers should treat that as "unknown" /
// hide the counter rather than show zero.
long onlineCountGlobal();

// Tear down the heartbeat thread + Redis connection. Called from
// main.cc's runOnQuit alongside PgListener / EmailHelper shutdown.
void stop();

} // namespace presence
