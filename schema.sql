-- Schema pentru blogul Drogon cu PostgreSQL

-- Funcția generică pentru menținerea coloanei updated_at
CREATE OR REPLACE FUNCTION set_updated_at()
RETURNS TRIGGER AS $$
BEGIN
    NEW.updated_at = NOW();
    RETURN NEW;
END;
$$ LANGUAGE plpgsql;

-- Tabela utilizatori
CREATE TABLE IF NOT EXISTS users (
    id                          INTEGER       GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    username                    VARCHAR(64)   UNIQUE NOT NULL,
    email                       VARCHAR(255)  UNIQUE NOT NULL,
    password_hash               TEXT          NOT NULL,
    profile_image               TEXT          DEFAULT NULL,
    bio                         TEXT          DEFAULT '',
    email_verified              INTEGER       DEFAULT 0,
    email_verification_token    TEXT          DEFAULT NULL,
    email_verification_expires  TIMESTAMP     DEFAULT NULL,
    created_at                  TIMESTAMP     NOT NULL DEFAULT NOW(),
    updated_at                  TIMESTAMP     NOT NULL DEFAULT NOW()
);

DROP TRIGGER IF EXISTS trg_users_updated_at ON users;
CREATE TRIGGER trg_users_updated_at
BEFORE UPDATE ON users
FOR EACH ROW EXECUTE FUNCTION set_updated_at();

-- Posts table. The `search` column is a GENERATED tsvector kept in sync with
-- title + content automatically (no triggers needed). title is weighted higher
-- (A) than content (B) so matches in the title rank above body matches.
CREATE TABLE IF NOT EXISTS posts (
    id          INTEGER     GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    user_id     INTEGER     NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    title       TEXT        NOT NULL,
    content     TEXT        NOT NULL,
    content_html TEXT,
    created_at  TIMESTAMP   NOT NULL DEFAULT NOW(),
    updated_at  TIMESTAMP   NOT NULL DEFAULT NOW(),
    search      TSVECTOR    GENERATED ALWAYS AS (
        setweight(to_tsvector('english', coalesce(title,   '')), 'A') ||
        setweight(to_tsvector('english', coalesce(content, '')), 'B')
    ) STORED
);

-- In-place migration for existing deployments where `posts` already exists
-- without the FTS column.
ALTER TABLE posts ADD COLUMN IF NOT EXISTS search TSVECTOR
    GENERATED ALWAYS AS (
        setweight(to_tsvector('english', coalesce(title,   '')), 'A') ||
        setweight(to_tsvector('english', coalesce(content, '')), 'B')
    ) STORED;

-- Rendered HTML for posts. Populated server-side at write time by the
-- Markdown helper (cmark-gfm, safe mode). Nullable so existing rows from
-- pre-markdown deployments keep working — the API falls back to escaped
-- plain text when the column is NULL.
ALTER TABLE posts ADD COLUMN IF NOT EXISTS content_html TEXT;

DROP TRIGGER IF EXISTS trg_posts_updated_at ON posts;
CREATE TRIGGER trg_posts_updated_at
BEFORE UPDATE ON posts
FOR EACH ROW EXECUTE FUNCTION set_updated_at();

-- Tabela comentarii
CREATE TABLE IF NOT EXISTS comments (
    id          INTEGER     GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    post_id     INTEGER     NOT NULL REFERENCES posts(id) ON DELETE CASCADE,
    user_id     INTEGER     NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    content     TEXT        NOT NULL,
    created_at  TIMESTAMP   NOT NULL DEFAULT NOW()
);

-- Tabela aprecieri (likes)
CREATE TABLE IF NOT EXISTS likes (
    id          INTEGER     GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    post_id     INTEGER     NOT NULL REFERENCES posts(id) ON DELETE CASCADE,
    user_id     INTEGER     NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    created_at  TIMESTAMP   NOT NULL DEFAULT NOW(),
    UNIQUE(post_id, user_id)
);

-- Tabela mesaje
CREATE TABLE IF NOT EXISTS messages (
    id           INTEGER    GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    sender_id    INTEGER    NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    receiver_id  INTEGER    NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    content      TEXT       NOT NULL,
    is_read      INTEGER    NOT NULL DEFAULT 0,
    created_at   TIMESTAMP  NOT NULL DEFAULT NOW()
);

-- Tabelă pentru reset password tokens
CREATE TABLE IF NOT EXISTS password_reset_tokens (
    id          INTEGER     GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    user_id     INTEGER     NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    token       TEXT        NOT NULL,
    expires_at  TIMESTAMP   NOT NULL,
    created_at  TIMESTAMP   NOT NULL DEFAULT NOW()
);

-- Indexuri pentru performanță
CREATE INDEX IF NOT EXISTS idx_posts_user_id           ON posts(user_id);
CREATE INDEX IF NOT EXISTS idx_posts_created_at        ON posts(created_at DESC);
CREATE INDEX IF NOT EXISTS idx_comments_post_id        ON comments(post_id);
CREATE INDEX IF NOT EXISTS idx_comments_user_id        ON comments(user_id);
CREATE INDEX IF NOT EXISTS idx_likes_post_id           ON likes(post_id);
CREATE INDEX IF NOT EXISTS idx_likes_user_id           ON likes(user_id);
CREATE INDEX IF NOT EXISTS idx_messages_sender         ON messages(sender_id);
CREATE INDEX IF NOT EXISTS idx_messages_receiver       ON messages(receiver_id);
CREATE INDEX IF NOT EXISTS idx_password_reset_token    ON password_reset_tokens(token);
CREATE INDEX IF NOT EXISTS idx_password_reset_user     ON password_reset_tokens(user_id);
CREATE INDEX IF NOT EXISTS idx_posts_search            ON posts USING GIN(search);

-- LISTEN/NOTIFY fan-out. Triggers on INSERT into messages and comments
-- emit a single NOTIFY on channel `blog_event` with the row enriched
-- (sender / author joined inline) so the C++ listener can push to WS
-- clients without an extra round-trip. Decoupling write -> fan-out via
-- the database makes the architecture multi-instance-ready: any process
-- LISTEN-ing on `blog_event` receives the event regardless of which
-- instance accepted the original POST. Defined at the very end of the
-- schema so the referenced tables (`messages`, `comments`) already exist.

CREATE OR REPLACE FUNCTION notify_new_message() RETURNS TRIGGER AS $$
DECLARE
    sender_row RECORD;
BEGIN
    SELECT id, username, profile_image
      INTO sender_row
      FROM users
     WHERE id = NEW.sender_id;

    PERFORM pg_notify('blog_event', json_build_object(
        'kind',        'message',
        'id',          NEW.id,
        'sender_id',   NEW.sender_id,
        'receiver_id', NEW.receiver_id,
        'content',     NEW.content,
        'is_read',     NEW.is_read,
        'created_at',  NEW.created_at::text,
        'sender', json_build_object(
            'id',            sender_row.id,
            'username',      sender_row.username,
            'profile_image', sender_row.profile_image
        )
    )::text);
    RETURN NEW;
END;
$$ LANGUAGE plpgsql;

DROP TRIGGER IF EXISTS trg_messages_notify ON messages;
CREATE TRIGGER trg_messages_notify
AFTER INSERT ON messages
FOR EACH ROW EXECUTE FUNCTION notify_new_message();

CREATE OR REPLACE FUNCTION notify_new_comment() RETURNS TRIGGER AS $$
DECLARE
    author_row RECORD;
BEGIN
    SELECT id, username, profile_image
      INTO author_row
      FROM users
     WHERE id = NEW.user_id;

    PERFORM pg_notify('blog_event', json_build_object(
        'kind',       'comment',
        'id',         NEW.id,
        'post_id',    NEW.post_id,
        'content',    NEW.content,
        'created_at', NEW.created_at::text,
        'author', json_build_object(
            'id',            author_row.id,
            'username',      author_row.username,
            'profile_image', author_row.profile_image
        )
    )::text);
    RETURN NEW;
END;
$$ LANGUAGE plpgsql;

DROP TRIGGER IF EXISTS trg_comments_notify ON comments;
CREATE TRIGGER trg_comments_notify
AFTER INSERT ON comments
FOR EACH ROW EXECUTE FUNCTION notify_new_comment();
