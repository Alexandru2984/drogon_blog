-- Session registry, so a user can see where their account is signed in and
-- cut off anything they do not recognise.
--
-- Drogon owns the sessions themselves (in-memory, keyed by the session
-- cookie) and offers no way to enumerate them, so until now a stolen
-- session could only be dealt with by waiting out the 14-day timeout.
-- There was no "sign out everywhere", and changing a password did nothing
-- to an attacker already holding a live session.
--
-- This table is a *shadow* of Drogon's store, not the store itself. `sid`
-- is a random token minted at login and kept inside the session payload —
-- it is NOT the session cookie. Holding a sid grants nothing: authentication
-- still requires the cookie. That is deliberate, and why the column is
-- plaintext rather than hashed like the email / reset tokens, which *are*
-- bearer credentials.
--
-- Rows outlive the sessions they describe. Drogon's store is in-memory, so
-- a process restart silently invalidates every session; sessions::install()
-- marks the leftovers revoked at startup so this table never advertises a
-- session that can no longer be used.

CREATE TABLE IF NOT EXISTS user_sessions (
    sid           TEXT        PRIMARY KEY,
    user_id       INTEGER     NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    created_at    TIMESTAMP   NOT NULL DEFAULT NOW(),
    last_seen_at  TIMESTAMP   NOT NULL DEFAULT NOW(),
    ip            TEXT        DEFAULT NULL,
    user_agent    TEXT        DEFAULT NULL,
    revoked_at    TIMESTAMP   DEFAULT NULL,
    -- Why the session ended, for the UI: 'user' (revoked from the session
    -- list), 'password_change', 'logout', 'restart'. NULL while live.
    revoked_reason TEXT       DEFAULT NULL
);

-- The session list is always "this user's live sessions". A partial index
-- keeps revoked rows out of it entirely — they are only ever read by the
-- pruning job, which scans by date instead.
CREATE INDEX IF NOT EXISTS idx_user_sessions_active
    ON user_sessions (user_id, last_seen_at DESC)
    WHERE revoked_at IS NULL;

-- Pruning predicate for scripts/cleanup.sh.
CREATE INDEX IF NOT EXISTS idx_user_sessions_revoked_at
    ON user_sessions (revoked_at)
    WHERE revoked_at IS NOT NULL;

-- Revocation has to reach every process, not just the one that handled the
-- request: each keeps its own in-memory set of revoked sids so the
-- per-request check costs a hash lookup instead of a query. Reuse the
-- existing blog_event channel that comments, messages and feature flags
-- already ride on, so there is one listener to reason about.
CREATE OR REPLACE FUNCTION notify_session_revoked() RETURNS TRIGGER AS $$
BEGIN
    IF NEW.revoked_at IS NOT NULL AND OLD.revoked_at IS NULL THEN
        PERFORM pg_notify('blog_event', json_build_object(
            'kind', 'session_revoked',
            'sid',  NEW.sid
        )::text);
    END IF;
    RETURN NEW;
END;
$$ LANGUAGE plpgsql;

DROP TRIGGER IF EXISTS trg_user_sessions_revoked ON user_sessions;
CREATE TRIGGER trg_user_sessions_revoked
AFTER UPDATE ON user_sessions
FOR EACH ROW EXECUTE FUNCTION notify_session_revoked();
