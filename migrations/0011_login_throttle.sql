-- Persistent per-account login throttle.
--
-- There is already a per-username token bucket in helpers/Security.cc (10
-- burst, 10 per 10 minutes) covering the case the per-IP limit misses: a
-- credential-stuffer rotating source addresses against one account. It
-- lives in process memory, which means every deploy resets it — and this
-- application deploys often enough for that to be a real gap, not a
-- theoretical one. An attacker who noticed the pattern could simply wait
-- for a restart.
--
-- These columns give the same limit a home that survives restarts.
--
-- On the deliberate-lockout risk: throttling by account is a way to lock a
-- victim out of their own account by failing logins on their behalf. That
-- is why the delay is capped at 15 minutes rather than escalating without
-- bound, and why crossing the threshold sends the account owner an email —
-- an attack that denies service should at least be visible to the person
-- it targets. The alternative, no per-account limit at all, hands
-- distributed credential stuffing a free pass, which is the worse trade.

ALTER TABLE users
    ADD COLUMN IF NOT EXISTS failed_login_count INTEGER   NOT NULL DEFAULT 0,
    ADD COLUMN IF NOT EXISTS last_failed_login  TIMESTAMP DEFAULT NULL,
    -- Set when the throttle first engages, so the "someone is trying to get
    -- into your account" email goes out once per episode rather than on
    -- every subsequent attempt.
    ADD COLUMN IF NOT EXISTS throttle_notified_at TIMESTAMP DEFAULT NULL;
