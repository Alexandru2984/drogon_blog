-- Switch the feature_flags NOTIFY channel from a dedicated
-- `flags_changed` channel to the existing `blog_event` channel that
-- PgListener already subscribes to. main.cc's existing dispatcher
-- gains a `kind="flag_changed"` branch that drops the in-memory flag
-- cache.
--
-- Reasoning: the helpers/PgListener implementation is single-channel
-- per process. Adding a second listener thread for one rarely-mutated
-- table is overkill — folding flag mutations into the same JSON-tagged
-- bus that already carries `message` and `comment` events is one less
-- moving part to operate.

CREATE OR REPLACE FUNCTION notify_flag_change()
RETURNS TRIGGER AS $$
BEGIN
    PERFORM pg_notify('blog_event',
        json_build_object(
            'kind', 'flag_changed',
            'key',  COALESCE(NEW.key, OLD.key)
        )::text);
    RETURN COALESCE(NEW, OLD);
END;
$$ LANGUAGE plpgsql;
