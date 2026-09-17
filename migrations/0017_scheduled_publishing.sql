-- Scheduled publishing.
--
-- A scheduled post is an ordinary draft (published_at IS NULL) that also
-- carries a scheduled_at timestamp. It stays invisible on every read path
-- exactly like any other draft — no visibility predicate anywhere changes —
-- until the scheduler (helpers/Scheduler) flips it live: sets published_at =
-- now(), clears scheduled_at, and notifies followers, the same transition a
-- manual publish makes. Keeping the "published = published_at IS NOT NULL"
-- invariant is the whole point: it means this feature cannot accidentally
-- leak a future-dated post into a feed.
ALTER TABLE posts
    ADD COLUMN IF NOT EXISTS scheduled_at TIMESTAMPTZ DEFAULT NULL;

-- The sweeper's hot query is "drafts whose time has come": a partial index on
-- the scheduled rows keeps it O(due rows) rather than a scan of every draft.
CREATE INDEX IF NOT EXISTS idx_posts_scheduled
    ON posts (scheduled_at)
    WHERE published_at IS NULL AND scheduled_at IS NOT NULL;
