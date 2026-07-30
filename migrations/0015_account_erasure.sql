-- Account erasure: the schema half of "delete my account".
--
-- The blog had no way to leave it. There was no delete endpoint at all, so
-- the only exit was asking the operator to run DELETE by hand — which, in a
-- jurisdiction where Article 17 applies, is not a process, it is an absence
-- of one.
--
-- The obvious implementation is `DELETE FROM users WHERE id = $1` and
-- letting the ON DELETE CASCADE fan out. That is wrong here, in two
-- specific ways:
--
--   1. comments.parent_id is ON DELETE CASCADE, so removing one comment in
--      the middle of a thread removes every reply underneath it. Those
--      replies belong to other people. Erasing one person's data must not
--      erase four other people's.
--
--   2. Every FK pointing at users would have to be re-checked on each new
--      table anyone adds, forever. A deletion path that silently misses a
--      table is worse than no deletion path, because it looks like it
--      worked.
--
-- So erasure anonymises the users row and deletes the content, rather than
-- deleting the row. What survives is one record with no personal data in
-- it: no name, no address, no password, no picture, no biography. What it
-- exists for is referential integrity — a tombstoned comment in someone
-- else's thread still needs a user_id, because the column is NOT NULL.
--
-- The original username and email are freed by the anonymisation, so a
-- person who deletes their account can register the same name again.

ALTER TABLE users
    ADD COLUMN IF NOT EXISTS deleted_at TIMESTAMP DEFAULT NULL;

COMMENT ON COLUMN users.deleted_at IS
    'Set when the account was erased. The row is a tombstone: username and '
    'email are anonymised, the password hash cannot verify, and every read '
    'path treats it as absent.';

-- Partial: deleted accounts are the rare case, and every read path that
-- cares asks "is this one deleted", never "list the deleted ones".
CREATE INDEX IF NOT EXISTS idx_users_deleted
    ON users (id) WHERE deleted_at IS NOT NULL;

-- ---------------------------------------------------------------- comments
--
-- A comment written by the departing user that has replies underneath it
-- cannot simply be deleted (see above), so it is tombstoned: the text is
-- replaced and the row is marked. It stays attached to the anonymised user
-- row, which by then carries no personal data.
--
-- Marking it explicitly rather than sniffing for a magic content string:
-- a reader is entirely capable of typing "[deleted]" into a comment box.

ALTER TABLE comments
    ADD COLUMN IF NOT EXISTS deleted_at TIMESTAMP DEFAULT NULL;

COMMENT ON COLUMN comments.deleted_at IS
    'Set when the author erased their account and this comment had replies. '
    'Content is replaced with a placeholder; the row survives so the replies '
    'under it do.';

-- ------------------------------------------------------------- audit trail
--
-- audit_log.actor_id is ON DELETE SET NULL, which was right when the row
-- disappeared. It no longer does, so the trail keeps pointing at the
-- tombstone — which is what an operator investigating an incident wants,
-- and contains no personal data either way.
--
-- The erasure itself is audited like any other sensitive action; there is
-- no new table for it.
