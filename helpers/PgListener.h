#pragma once

#include <functional>
#include <string>

namespace pglisten {

// Callback fired on each NOTIFY received on the LISTEN-ed channel.
// `payload` is the raw NOTIFY payload (typically a JSON string emitted by
// the trigger). Runs on the PgListener's own worker thread — implementers
// must dispatch any per-EventLoop work back to Drogon themselves.
using Callback = std::function<void(const std::string& channel,
                                    const std::string& payload)>;

// Start a single background thread that opens a dedicated libpq connection,
// LISTENs on the given channel, and invokes `cb` for every incoming
// notification. Idempotent — second call is a no-op.
//
// `connInfo` is a libpq connection string. We build it from the same
// DB_HOST/DB_PORT/DB_NAME/DB_USER/DB_PASSWORD env vars used everywhere
// else, so callers can leave it empty to rely on env-driven defaults.
void start(const std::string& channel,
           Callback           cb,
           const std::string& connInfo = "");

// Stop the listener and join the thread. Safe to call multiple times.
void stop();

} // namespace pglisten
