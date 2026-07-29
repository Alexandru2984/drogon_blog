-- Tags, drafts, reading time and view counts.
--
-- The blog could do exactly one thing with a post: publish it, immediately,
-- into a single flat reverse-chronological feed. There was no way to save
-- something half-written, no way to group related posts, and no signal at
-- all about how long a post takes to read or whether anyone had read it.
--
-- Four pieces, all additive — every existing row keeps working without a
-- backfill pass:
--
--   1. posts.published_at — NULL means draft. A nullable timestamp rather
--      than a boolean because "when did this go live" is a question the
--      feed needs to answer anyway (it orders by it), and a boolean would
--      leave that ordering keyed on created_at, which for a post drafted in
--      March and published in July is the wrong date.
--
--   2. tags + post_tags — a many-to-many, with the display form kept
--      alongside a normalised slug so "C++", "c++" and "C ++" cannot become
--      three separate tags.
--
--   3. posts.reading_minutes — computed once on write rather than on every
--      read. It is a function of the content, so recomputing it per request
--      is work that produces the same answer every time.
--
--   4. post_views — one row per (post, viewer-identity, day). Deduplicating
--      in the schema rather than in the application is what stops a refresh
--      loop from inflating the count, and doing it per day rather than
--      forever means a genuine return visit still registers.

-- ---------------------------------------------------------------- drafts

ALTER TABLE posts
    ADD COLUMN IF NOT EXISTS published_at    TIMESTAMP DEFAULT NULL,
    ADD COLUMN IF NOT EXISTS reading_minutes INTEGER   NOT NULL DEFAULT 1,
    ADD COLUMN IF NOT EXISTS excerpt         TEXT      DEFAULT NULL;

-- Everything that already exists was published the moment it was created —
-- there was no other possibility before this migration. Without this, the
-- entire archive would become drafts and vanish from the feed.
UPDATE posts SET published_at = created_at WHERE published_at IS NULL;

-- The feed's hot path is "published, not hidden, newest first". A partial
-- index over exactly that predicate keeps drafts and hidden posts out of the
-- scan entirely rather than filtering them after the fact.
CREATE INDEX IF NOT EXISTS idx_posts_published
    ON posts (published_at DESC, id DESC)
    WHERE published_at IS NOT NULL AND hidden_at IS NULL;

-- An author listing their own drafts is the only query that wants the
-- opposite predicate.
CREATE INDEX IF NOT EXISTS idx_posts_drafts
    ON posts (user_id, updated_at DESC)
    WHERE published_at IS NULL;

-- ---------------------------------------------------------------- tags

CREATE TABLE IF NOT EXISTS tags (
    id          INTEGER GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    -- The lowercase, punctuation-folded form. UNIQUE here is what makes
    -- "C++" and "c++" the same tag; the application normalises, the
    -- constraint enforces.
    slug        TEXT NOT NULL UNIQUE,
    -- What the first author to use it actually typed, for display.
    label       TEXT NOT NULL,
    created_at  TIMESTAMP NOT NULL DEFAULT now(),
    CONSTRAINT tags_slug_shape CHECK (slug ~ '^[a-z0-9][a-z0-9-]{0,38}$'),
    CONSTRAINT tags_label_len  CHECK (char_length(label) BETWEEN 1 AND 40)
);

CREATE TABLE IF NOT EXISTS post_tags (
    post_id INTEGER NOT NULL REFERENCES posts(id) ON DELETE CASCADE,
    tag_id  INTEGER NOT NULL REFERENCES tags(id)  ON DELETE CASCADE,
    PRIMARY KEY (post_id, tag_id)
);

-- The PK covers post -> tags. This covers tags -> posts, which is the
-- browse-by-tag direction.
CREATE INDEX IF NOT EXISTS idx_post_tags_tag ON post_tags (tag_id, post_id DESC);

-- ---------------------------------------------------------------- views

CREATE TABLE IF NOT EXISTS post_views (
    post_id   INTEGER NOT NULL REFERENCES posts(id) ON DELETE CASCADE,
    -- Who, as far as we are willing to know. For a signed-in reader this is
    -- 'u:<id>'. For anonymous readers it is 'a:' plus a truncated keyed hash
    -- of the client IP and User-Agent — enough to deduplicate a refresh,
    -- not enough to identify anyone or to link two days together, since the
    -- day is part of the row and the hash is salted per deployment.
    viewer    TEXT NOT NULL,
    viewed_on DATE NOT NULL DEFAULT CURRENT_DATE,
    PRIMARY KEY (post_id, viewer, viewed_on)
);

-- Counting views for one post must not scan the whole table.
CREATE INDEX IF NOT EXISTS idx_post_views_post ON post_views (post_id);

-- Old rows have no value once counted: they exist only to deduplicate
-- within a day. scripts/cleanup.sh prunes them; this index makes that cheap.
CREATE INDEX IF NOT EXISTS idx_post_views_day ON post_views (viewed_on);

-- A denormalised running total, so rendering a post or a feed page does not
-- aggregate post_views. Incremented by the same statement that inserts the
-- dedup row, inside one transaction, so the two cannot drift.
ALTER TABLE posts
    ADD COLUMN IF NOT EXISTS view_count BIGINT NOT NULL DEFAULT 0;

-- posts carries a BEFORE UPDATE trigger that stamps updated_at. Bumping
-- view_count is an UPDATE, so without a guard every single read would set
-- updated_at = now(). Two things break when that happens:
--
--   * updated_at stops meaning "when was this last edited" and starts
--     meaning "when was this last looked at", which is what the feed's
--     ordering, the ETag and the sitemap's <lastmod> all read it as.
--   * Every ETag derived from it changes on every view, so no reader ever
--     gets a 304 and the post's cache entry is invalidated continuously —
--     including for readers whose own request was not the one that bumped
--     it.
--
-- The WHEN clause narrows the trigger to updates that changed something
-- other than the counter. Nothing writes view_count together with a real
-- edit, so there is no case where a genuine change is missed.
DROP TRIGGER IF EXISTS trg_posts_updated_at ON posts;
CREATE TRIGGER trg_posts_updated_at
BEFORE UPDATE ON posts
FOR EACH ROW
WHEN (OLD.view_count IS NOT DISTINCT FROM NEW.view_count)
EXECUTE FUNCTION set_updated_at();
