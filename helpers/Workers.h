#pragma once

#include <drogon/HttpResponse.h>

#include <cstddef>
#include <functional>
#include <string>

// Thread pools for work that must not run on a Drogon event loop.
//
// Drogon dispatches every request onto one of `number_of_threads` IO loops
// (12 on the production host) and runs the handler inline on that loop. A
// handler that blocks therefore stalls *every* connection pinned to that
// loop — not just its own request — until it returns. Two families of
// blocking work were doing exactly that:
//
//   Argon2id. Measured on the production host at 167 ms to hash and 158 ms
//   to verify, by design: the cost is the defence. Every login, every
//   registration and every password re-auth paid it on an event loop.
//   Issuing a batch of recovery codes hashes ten of them in a row, ~1.7 s,
//   and AuthController2fa additionally runs 29 execSqlSync calls, each
//   parking the loop on a database round-trip.
//
//   libvips. Decode + resize + re-encode of an 8 MB upload, on the loop,
//   for as long as it takes.
//
// The fix is to move the blocking body to a worker thread and let the loop
// go back to serving. Drogon's response callback is safe to invoke from any
// thread — the codebase already does so from database threads via
// execSqlAsync — so a handler can hand its work to a pool and reply from
// there.
//
// This is deliberately not a rewrite of those handlers into async
// continuations. Converting 29 synchronous queries across the 2FA flows
// into nested callbacks would be a large, delicate change to the most
// security-sensitive code in the app; running the same code on a thread
// that is allowed to block gets the same result without touching the
// logic.
namespace workers {

// Two pools, not one, because the work has different shapes and the auth
// path is latency-sensitive. A single queue would let a couple of slow
// image jobs sit in front of a login.
enum class Pool {
    Auth,    // Argon2id + the synchronous DB calls in the auth / 2FA flows
    Media,   // libvips decode / resize / encode
};

// Starts the pools. Call once, before app().run().
//
// Sizes default to 4 (Auth) and 2 (Media) and are overridable via
// BLOG_AUTH_WORKERS / BLOG_MEDIA_WORKERS. The Auth default is a memory
// bound as much as a CPU one: libsodium's INTERACTIVE limit is 64 MiB per
// concurrent hash, so 4 workers cap Argon2id at 256 MiB no matter how many
// logins arrive at once.
void start();

// Drains in-flight jobs and joins the threads. Call from runOnQuit.
void stop();

// Queues `job` on `pool`. Returns false without queueing when the pool's
// backlog is full, which is the backpressure signal: a caller that cannot
// be served should be told so immediately rather than after a queue-long
// wait. Also returns false once stop() has run.
bool submit(Pool pool, std::function<void()> job);

// submit(), but answers `callback` with a 503 + Retry-After when the pool
// is saturated, so handlers do not each re-invent the rejection response.
// Returns true when the job was queued.
//
// Usage inside a handler:
//     if (!workers::offload(workers::Pool::Auth, callback,
//             [req, callback] { ...blocking work...; callback(resp); }))
//         return;
bool offload(Pool pool,
             const std::function<void(const drogon::HttpResponsePtr&)>& callback,
             std::function<void()> job);

// Introspection for /metrics and tests.
struct Stats {
    std::size_t threads;
    std::size_t queued;     // jobs waiting for a worker
    std::size_t active;     // jobs currently running
    std::size_t capacity;   // max backlog before submit() refuses
    std::size_t rejected;   // cumulative submit() refusals
};
Stats stats(Pool pool);

const char* poolName(Pool pool);

} // namespace workers
