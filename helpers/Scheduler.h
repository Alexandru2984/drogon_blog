#pragma once

#include <cstddef>

// Publishes posts whose scheduled_at has arrived.
//
// A scheduled post is an ordinary draft (published_at IS NULL) carrying a
// scheduled_at timestamp (see migrations/0017_scheduled_publishing.sql). The
// background sweep below is the only thing that turns it into a live post —
// setting published_at = now(), clearing scheduled_at, and notifying followers,
// exactly the transition a manual publish makes. Until it fires the post is
// invisible on every read path like any draft, so this feature adds no new
// visibility predicate anywhere.
namespace scheduler {

// Spawn the sweep thread (idempotent). Sweeps every 60s.
void start();

// Signal the thread to stop and join it.
void stop();

// Run one sweep synchronously and return how many posts were published.
// Shared by the loop and by tests, which need a deterministic trigger rather
// than waiting on the timer.
std::size_t publishDueNow();

} // namespace scheduler
