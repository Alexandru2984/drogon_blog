-- Comments table originally tracked only created_at. The PUT endpoint
-- (CommentController::updateComment) mutates `content` in place, so
-- the only visible field that changes on edit has no timestamp behind
-- it — which breaks any cache layer that wants a stable ETag for the
-- per-post comment list. A client could fetch /posts/N/comments, an
-- author then edits a comment, and the next fetch would 304 against
-- the stale list because every fragment of the ETag (post_id, count,
-- max(created_at)) is unchanged.
--
-- Mirror what posts already does: an updated_at column defaulting to
-- NOW(), backfilled to created_at for pre-existing rows, and a
-- BEFORE-UPDATE trigger reusing the global set_updated_at() function
-- from 0001_init.sql.

ALTER TABLE comments
    ADD COLUMN IF NOT EXISTS updated_at TIMESTAMP NOT NULL DEFAULT NOW();

-- Backfill: every existing row gets its created_at copied over so the
-- column is meaningful from day one (not all "now" / current timestamp
-- for rows that were actually written long ago).
UPDATE comments SET updated_at = created_at WHERE updated_at > created_at;

DROP TRIGGER IF EXISTS trg_comments_updated_at ON comments;
CREATE TRIGGER trg_comments_updated_at
BEFORE UPDATE ON comments
FOR EACH ROW EXECUTE FUNCTION set_updated_at();
