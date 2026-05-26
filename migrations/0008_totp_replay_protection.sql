-- TOTP replay protection.
--
-- RFC 6238 codes stay valid for a whole time-step (and we accept a ±1
-- window for clock skew), so a single 6-digit code is replayable for up
-- to ~90 s. The per-account rate limit caps the volume of attempts but
-- does not stop a captured-then-replayed code inside that window — e.g.
-- a code phished and immediately reused, or a login-verify request
-- replayed off a mirrored TLS-terminating proxy.
--
-- Fix: remember the last time-step we accepted for each secret and refuse
-- any code whose matched step is <= that value. Strictly monotonic, so a
-- code can be accepted exactly once. Backfilled to 0 (1970) which is below
-- every real step, so existing enrollments keep working — the first login
-- after this migration simply seeds the column.
ALTER TABLE user_totp_secrets
    ADD COLUMN IF NOT EXISTS last_used_step BIGINT NOT NULL DEFAULT 0;
