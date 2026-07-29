-- Threaded comments, bookmarks, follows and notifications.
--
-- Everything here exists to answer one question the blog could not answer:
-- "what happened while I was away?" A reader had no way to save a post for
-- later, no way to hear about a new post from an author they liked, and no
-- way to know that someone had replied to them — replies were not even a
-- thing, since every comment was a flat sibling of every other.
--
-- Four pieces:
--
--   1. comments.parent_id — a self-reference making a comment a reply.
--   2. bookmarks — a reader's own reading list.
--   3. follows — author subscriptions, which is what makes a personalised
--      feed and new-post notifications possible.
--   4. notifications — the delivery surface for all of the above.

-- ------------------------------------------------------------- threading

ALTER TABLE comments
    ADD COLUMN IF NOT EXISTS parent_id INTEGER DEFAULT NULL
        REFERENCES comments(id) ON DELETE CASCADE;

-- Deliberately CASCADE, not SET NULL. A reply whose parent is gone has lost
-- the thing it was replying to; promoting it to a top-level comment puts an
-- answer with no question in front of every reader, and the answer often
-- only makes sense as a reply ("I disagree", "same here"). Deleting the
-- subtree is the honest outcome.

-- Fetching one post's thread reads parent_id constantly; without this the
-- planner has only the post_id index and re-scans for each parent.
CREATE INDEX IF NOT EXISTS idx_comments_parent
    ON comments (parent_id, id)
    WHERE parent_id IS NOT NULL;

-- ------------------------------------------------------------- bookmarks

CREATE TABLE IF NOT EXISTS bookmarks (
    user_id    INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    post_id    INTEGER NOT NULL REFERENCES posts(id) ON DELETE CASCADE,
    created_at TIMESTAMP NOT NULL DEFAULT now(),
    -- The PK is the uniqueness constraint: bookmarking twice is idempotent
    -- rather than an error the client has to handle.
    PRIMARY KEY (user_id, post_id)
);

-- "Is this post bookmarked by me" is answered by the PK. This covers the
-- other direction — the reading list itself, newest first.
CREATE INDEX IF NOT EXISTS idx_bookmarks_user
    ON bookmarks (user_id, created_at DESC);

-- --------------------------------------------------------------- follows

CREATE TABLE IF NOT EXISTS follows (
    follower_id INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    followee_id INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    created_at  TIMESTAMP NOT NULL DEFAULT now(),
    PRIMARY KEY (follower_id, followee_id),
    -- Following yourself would put your own posts in your "from people you
    -- follow" feed, which is not what that feed is for. Enforced here so no
    -- code path can create it.
    CONSTRAINT follows_no_self CHECK (follower_id <> followee_id)
);

-- The PK serves follower -> followees (my feed). This serves the reverse:
-- "who follows this author", which is what fans out a new post.
CREATE INDEX IF NOT EXISTS idx_follows_followee
    ON follows (followee_id, follower_id);

-- --------------------------------------------------------- notifications

CREATE TABLE IF NOT EXISTS notifications (
    id         BIGINT GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    -- Who sees it.
    user_id    INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    -- Who caused it. Nullable because a notification can outlive the
    -- account that triggered it, and losing the whole notification when
    -- someone deletes their account would silently rewrite history.
    actor_id   INTEGER REFERENCES users(id) ON DELETE SET NULL,
    kind       TEXT    NOT NULL,
    -- The thing to navigate to. Both nullable: a follow notification points
    -- at a user, not a post.
    post_id    INTEGER REFERENCES posts(id)    ON DELETE CASCADE,
    comment_id INTEGER REFERENCES comments(id) ON DELETE CASCADE,
    read_at    TIMESTAMP DEFAULT NULL,
    created_at TIMESTAMP NOT NULL DEFAULT now(),
    -- Constrained rather than free text, for the same reason users.role is:
    -- a typo would create a kind no renderer matches, and the notification
    -- would arrive as a blank row.
    CONSTRAINT notifications_kind_check
        CHECK (kind IN ('comment', 'reply', 'follow', 'new_post', 'like'))
);

-- The two queries that exist: the notification list (newest first) and the
-- unread badge. A partial index for the badge keeps it off the full history.
CREATE INDEX IF NOT EXISTS idx_notifications_user
    ON notifications (user_id, id DESC);
CREATE INDEX IF NOT EXISTS idx_notifications_unread
    ON notifications (user_id)
    WHERE read_at IS NULL;

-- Cross-process delivery. The blog runs one process today, but the WebSocket
-- hub is per-process and main.cc already LISTENs on blog_event to fan out
-- comments and messages; putting notifications on the same channel means a
-- second process would work without further changes.
--
-- The payload is deliberately minimal — an id and a recipient. pg_notify
-- payloads are capped at 8000 bytes and a client that has to be told the
-- full contents cannot be trusted to have them anyway; the receiver reads
-- the row.
CREATE OR REPLACE FUNCTION notify_new_notification()
RETURNS TRIGGER AS $$
BEGIN
    -- 'kind' is the discriminator main.cc dispatches on, and it already
    -- has a 'comment' value for the new-comment fan-out. A notification of
    -- kind 'comment' must not be mistaken for one of those, so the envelope
    -- says 'notification' and the notification's own kind rides as
    -- 'notification_kind'.
    PERFORM pg_notify('blog_event', json_build_object(
        'kind',              'notification',
        'user_id',           NEW.user_id,
        'id',                NEW.id,
        'notification_kind', NEW.kind
    )::text);
    RETURN NEW;
END;
$$ LANGUAGE plpgsql;

DROP TRIGGER IF EXISTS trg_notifications_notify ON notifications;
CREATE TRIGGER trg_notifications_notify
AFTER INSERT ON notifications
FOR EACH ROW EXECUTE FUNCTION notify_new_notification();
