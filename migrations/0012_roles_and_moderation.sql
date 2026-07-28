-- Roles, moderation and abuse reporting.
--
-- The application had no notion of privilege at all: every account was
-- equal, so there was no way to deal with abusive content except by
-- editing the database by hand, and no way for a reader to report anything
-- in the first place. For a public site that accepts user-submitted posts,
-- comments and direct messages, that is a gap that closes itself the
-- unpleasant way.
--
-- Three pieces:
--
--   1. users.role — 'user', 'moderator' or 'admin'. Constrained rather
--      than free text so a typo cannot silently create a role that no
--      check matches (and therefore grants nothing, which at least fails
--      safe, but is confusing) or — worse — accidentally match a check
--      written against a different spelling.
--
--   2. Soft-hide columns on posts and comments. Moderation deletes are
--      reversible on purpose: a moderator acting on a report is making a
--      judgement call, and an irreversible action taken on incomplete
--      information is how moderation goes wrong. The content stays in the
--      table and stops being served.
--
--   3. reports — what a reader flagged, why, and what was decided.

ALTER TABLE users
    ADD COLUMN IF NOT EXISTS role         TEXT      NOT NULL DEFAULT 'user',
    ADD COLUMN IF NOT EXISTS banned_until TIMESTAMP DEFAULT NULL,
    ADD COLUMN IF NOT EXISTS ban_reason   TEXT      DEFAULT NULL;

DO $$
BEGIN
    IF NOT EXISTS (SELECT 1 FROM pg_constraint WHERE conname = 'users_role_check') THEN
        ALTER TABLE users ADD CONSTRAINT users_role_check
            CHECK (role IN ('user', 'moderator', 'admin'));
    END IF;
END $$;

-- Every privileged query filters on this, and the table is dominated by
-- ordinary users, so a partial index keeps it to the handful of rows that
-- are not 'user'.
CREATE INDEX IF NOT EXISTS idx_users_staff
    ON users (role) WHERE role <> 'user';

ALTER TABLE posts
    ADD COLUMN IF NOT EXISTS hidden_at     TIMESTAMP DEFAULT NULL,
    ADD COLUMN IF NOT EXISTS hidden_by     INTEGER   DEFAULT NULL REFERENCES users(id) ON DELETE SET NULL,
    ADD COLUMN IF NOT EXISTS hidden_reason TEXT      DEFAULT NULL;

ALTER TABLE comments
    ADD COLUMN IF NOT EXISTS hidden_at     TIMESTAMP DEFAULT NULL,
    ADD COLUMN IF NOT EXISTS hidden_by     INTEGER   DEFAULT NULL REFERENCES users(id) ON DELETE SET NULL,
    ADD COLUMN IF NOT EXISTS hidden_reason TEXT      DEFAULT NULL;

-- Feed, search and listing queries all gained `hidden_at IS NULL`. Partial
-- indexes on the visible rows keep those paths on an index scan; hidden
-- content is a rounding error by row count and does not need indexing.
CREATE INDEX IF NOT EXISTS idx_posts_visible
    ON posts (id DESC) WHERE hidden_at IS NULL;
CREATE INDEX IF NOT EXISTS idx_comments_visible
    ON comments (post_id, id DESC) WHERE hidden_at IS NULL;

-- Ban state is checked before every content mutation. Doing that with a
-- query per request would put a database round-trip on the write path to
-- cover something that happens a handful of times a year, so each process
-- keeps the banned set in memory and the check is a hash lookup. This
-- trigger is what keeps those copies honest across processes — same
-- blog_event channel the comments, messages, flags and session revocations
-- already ride on.
CREATE OR REPLACE FUNCTION notify_ban_changed() RETURNS TRIGGER AS $$
BEGIN
    IF NEW.banned_until IS DISTINCT FROM OLD.banned_until THEN
        PERFORM pg_notify('blog_event', json_build_object(
            'kind',    'user_ban_changed',
            'user_id', NEW.id,
            'banned',  (NEW.banned_until IS NOT NULL AND NEW.banned_until > NOW())
        )::text);
    END IF;
    RETURN NEW;
END;
$$ LANGUAGE plpgsql;

DROP TRIGGER IF EXISTS trg_users_ban_changed ON users;
CREATE TRIGGER trg_users_ban_changed
AFTER UPDATE ON users
FOR EACH ROW EXECUTE FUNCTION notify_ban_changed();

CREATE TABLE IF NOT EXISTS reports (
    id             INTEGER     GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    -- Who reported it. ON DELETE SET NULL rather than CASCADE: a report
    -- keeps its value after the reporter deletes their account, and losing
    -- the history would let someone erase evidence by leaving.
    reporter_id    INTEGER     REFERENCES users(id) ON DELETE SET NULL,
    target_type    TEXT        NOT NULL CHECK (target_type IN ('post', 'comment', 'user')),
    target_id      INTEGER     NOT NULL,
    reason         TEXT        NOT NULL CHECK (reason IN
                       ('spam', 'harassment', 'illegal', 'sexual', 'other')),
    detail         TEXT        NOT NULL DEFAULT '',
    status         TEXT        NOT NULL DEFAULT 'open'
                       CHECK (status IN ('open', 'actioned', 'dismissed')),
    resolved_by    INTEGER     REFERENCES users(id) ON DELETE SET NULL,
    resolved_at    TIMESTAMP   DEFAULT NULL,
    resolution_note TEXT       DEFAULT '',
    created_at     TIMESTAMP   NOT NULL DEFAULT NOW(),
    updated_at     TIMESTAMP   NOT NULL DEFAULT NOW()
);

DROP TRIGGER IF EXISTS trg_reports_updated_at ON reports;
CREATE TRIGGER trg_reports_updated_at
BEFORE UPDATE ON reports
FOR EACH ROW EXECUTE FUNCTION set_updated_at();

-- The moderation queue is "open reports, oldest first".
CREATE INDEX IF NOT EXISTS idx_reports_open
    ON reports (created_at) WHERE status = 'open';

-- One open report per reporter per target. Without this, a single user can
-- file the same complaint repeatedly and drown the queue, and the open
-- count stops meaning anything.
CREATE UNIQUE INDEX IF NOT EXISTS idx_reports_one_open_per_reporter
    ON reports (reporter_id, target_type, target_id)
    WHERE status = 'open' AND reporter_id IS NOT NULL;
