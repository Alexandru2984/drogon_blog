#pragma once

#include <optional>
#include <string>
#include <vector>

// Feature flags / A/B testing.
//
// Storage: a single `feature_flags` table populated out-of-band (a
// migration, a CLI, or `psql -c "INSERT…"`). The app never writes to
// this table on its own — there's no admin endpoint by design,
// because the rollout audit trail belongs in your deploy / database
// change-management system, not in HTTP logs.
//
// Bucketing: deterministic.
//   bucket = sha256(key || ":" || user_id) % 100
// A flag at 30 % rolls out to everyone whose bucket < 30. Bumping
// the percentage to 50 % grows the cohort monotonically — every
// previously-in user stays in. Anonymous callers pass user_id=0,
// which lives in its own bucket so guests are sampled at the same
// rate the percentage advertises.
//
// Invalidation: when the table mutates, the migration's trigger
// emits `pg_notify('blog_event', json{kind:"flag_changed",…})`.
// main.cc's existing dispatcher calls `flags::reload()` on receipt,
// which is a single SELECT into a swap-then-publish cache. No polling.
namespace flags {

struct Flag {
    std::string key;
    std::string description;
    bool        enabled         = false;
    int         rollout_percent = 0;
};

// Warm the cache from PG synchronously. Call at startup, AFTER
// drogon::app().getDbClient() is wired. Idempotent.
bool install();

// Drop+rebuild the cache from PG. Called by main.cc's pglisten
// dispatcher on `kind="flag_changed"` events. Safe to call from any
// thread.
void reload();

// True when `key` is enabled for the given userId. userId=0 ==
// anonymous. Unknown keys evaluate to false (fail-closed).
bool isEnabled(const std::string& key, int userId);

// Bulk evaluation for the `/flags` endpoint. Returns one entry per
// known flag, with its evaluated `enabled` for the caller.
struct EvalResult {
    std::string key;
    bool        enabled;
};
std::vector<EvalResult> evaluateAll(int userId);

// Single lookup variant (mostly for the `/flags/{key}` endpoint).
// std::nullopt → key not in the table.
std::optional<bool> lookup(const std::string& key, int userId);

} // namespace flags
