-- pg_notify caps payload at 8 KiB (NAMEDATALEN-1 * BLCKSZ math, hard
-- compile-time limit). The previous trigger inlined NEW.content
-- verbatim into json_build_object — a single comment/message at
-- 7-8 KiB would push the JSON envelope past the limit, raise
-- "payload string too long", and crash the entire INSERT (the
-- transaction rolls back, the POST returns 500). DoS via long body.
--
-- This migration:
--   * truncates NEW.content to 1000 bytes inside the trigger payload
--     (well clear of the 8 KiB cap even with worst-case JSON escaping
--     for non-ASCII content);
--   * adds a `content_truncated` boolean so the WS hub / frontend can
--     decide whether to refetch the full row via REST before showing
--     it (current behaviour is to use the preview directly, which is
--     fine as a graceful degradation — REST still returns the full
--     body on conversation open).
--
-- The stored row is unchanged: `messages.content` / `comments.content`
-- keep the full text. The application layer caps writes at 10 KiB
-- (kMaxMessageBytes / kMaxCommentBytes in the controllers) and the
-- markdown renderer caps post bodies at 100 KiB independently.

CREATE OR REPLACE FUNCTION notify_new_message() RETURNS TRIGGER AS $$
DECLARE
    sender_row RECORD;
    preview    TEXT;
    truncated  BOOLEAN;
BEGIN
    SELECT id, username, profile_image
      INTO sender_row
      FROM users
     WHERE id = NEW.sender_id;

    preview   := LEFT(NEW.content, 1000);
    truncated := length(NEW.content) > 1000;

    PERFORM pg_notify('blog_event', json_build_object(
        'kind',              'message',
        'id',                NEW.id,
        'sender_id',         NEW.sender_id,
        'receiver_id',       NEW.receiver_id,
        'content',           preview,
        'content_truncated', truncated,
        'is_read',           NEW.is_read,
        'created_at',        NEW.created_at::text,
        'sender', json_build_object(
            'id',            sender_row.id,
            'username',      sender_row.username,
            'profile_image', sender_row.profile_image
        )
    )::text);
    RETURN NEW;
END;
$$ LANGUAGE plpgsql;

CREATE OR REPLACE FUNCTION notify_new_comment() RETURNS TRIGGER AS $$
DECLARE
    author_row RECORD;
    preview    TEXT;
    truncated  BOOLEAN;
BEGIN
    SELECT id, username, profile_image
      INTO author_row
      FROM users
     WHERE id = NEW.user_id;

    preview   := LEFT(NEW.content, 1000);
    truncated := length(NEW.content) > 1000;

    PERFORM pg_notify('blog_event', json_build_object(
        'kind',              'comment',
        'id',                NEW.id,
        'post_id',           NEW.post_id,
        'content',           preview,
        'content_truncated', truncated,
        'created_at',        NEW.created_at::text,
        'author', json_build_object(
            'id',            author_row.id,
            'username',      author_row.username,
            'profile_image', author_row.profile_image
        )
    )::text);
    RETURN NEW;
END;
$$ LANGUAGE plpgsql;
