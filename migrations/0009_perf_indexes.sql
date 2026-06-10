-- Performance indexes for hot lookups that previously fell back to a
-- sequential scan or an out-of-index sort.
--
-- 1. verifyEmail() does `WHERE email_verification_token = $1`. The column had
--    no index, so every verification attempt seq-scanned users. A partial
--    index keeps it tiny — only the small set of rows with a pending token is
--    indexed, and verified accounts (token NULL) carry no index weight.
--
-- 2. The message list + conversation endpoints filter by receiver_id / sender_id
--    and order by id DESC. The old single-column indexes covered the filter but
--    not the ordering, forcing a sort per page. Composite (col, id DESC) indexes
--    serve both the equality filter and the ORDER BY/LIMIT directly, and their
--    leading column still backs the ON DELETE CASCADE from users. They strictly
--    supersede the single-column indexes, which are dropped.
--
-- CREATE INDEX (not CONCURRENTLY): apply.sh wraps each migration in a single
-- transaction, and CONCURRENTLY cannot run inside one. The tables are small so
-- the brief lock is acceptable.

CREATE INDEX IF NOT EXISTS idx_users_email_verification_token
    ON users (email_verification_token)
    WHERE email_verification_token IS NOT NULL;

CREATE INDEX IF NOT EXISTS idx_messages_receiver_id_desc
    ON messages (receiver_id, id DESC);

CREATE INDEX IF NOT EXISTS idx_messages_sender_id_desc
    ON messages (sender_id, id DESC);

DROP INDEX IF EXISTS idx_messages_receiver;
DROP INDEX IF EXISTS idx_messages_sender
