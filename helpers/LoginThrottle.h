#pragma once

#include <string>

// Persistent per-account login throttle.
//
// helpers/Security.cc already keeps a per-username token bucket, which
// covers what the per-IP limit misses: a credential-stuffer rotating
// source addresses against a single account. That bucket lives in process
// memory, so every deploy resets it — and this application deploys often
// enough that an attacker who noticed could simply wait for a restart to
// get a fresh budget. These counters live in the users table instead.
//
// On the obvious objection — that throttling by account lets an attacker
// lock a victim out by failing logins on their behalf — the mitigations
// are that the delay is capped rather than escalating without bound, that
// a correct password never advances the counter, and that crossing the
// threshold emails the account owner. An attack that denies service should
// at least be visible to the person it targets. Dropping the per-account
// limit entirely would hand distributed stuffing a free pass, which is the
// worse trade.
namespace login_throttle {

struct Decision {
    bool throttled          = false;
    int  retryAfterSeconds  = 0;
};

// Whether this account is currently refusing password attempts.
// Blocking — call from a worker thread.
Decision check(int userId);

// Records a failed password attempt and, when the threshold is first
// crossed, emails the account owner. Blocking.
void recordFailure(int userId,
                   const std::string& email,
                   const std::string& username);

// Clears the counters after a correct password. Blocking.
void recordSuccess(int userId);

// Thresholds, exposed so tests do not hardcode them separately.
// After `kThreshold` consecutive failures the account is throttled for
// `kWindowSeconds`, capped — the window does not grow past this.
constexpr int kThreshold     = 10;
constexpr int kWindowSeconds = 900;   // 15 minutes

} // namespace login_throttle
