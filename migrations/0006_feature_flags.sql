-- A/B testing + feature flags.
--
-- The table is intentionally minimal: a string key, a kill switch
-- (`enabled`), and a percentage rollout. Bucketing is computed in the
-- app via sha256(key || ":" || user_id) so the same (key, user) pair
-- always lands in the same bucket — flipping `rollout_percent` from
-- 30 → 50 grows the cohort monotonically rather than reshuffling who
-- was already in vs out.
--
-- Anonymous callers (no session) get user_id=0, which buckets them on
-- their own — a 50% rollout still serves the variant to 50% of guests,
-- not 0% or 100%.
--
-- Triggers:
--   * `set_updated_at` reuses the shared function from 0001_init.sql.
--   * `notify_flag_change` fires a pg_notify on every mutation. The
--     C++ side (helpers/Flags) LISTENs on `flags_changed` and reloads
--     its in-memory cache without polling. NOTIFY payload carries the
--     key so callers could optimise to a partial reload — for now the
--     handler just clears the whole cache, which is fine given the
--     table is tiny.

CREATE TABLE IF NOT EXISTS feature_flags (
    key              TEXT        PRIMARY KEY,
    description      TEXT        NOT NULL DEFAULT '',
    enabled          BOOLEAN     NOT NULL DEFAULT FALSE,
    rollout_percent  INTEGER     NOT NULL DEFAULT 0
        CHECK (rollout_percent BETWEEN 0 AND 100),
    created_at       TIMESTAMP   NOT NULL DEFAULT NOW(),
    updated_at       TIMESTAMP   NOT NULL DEFAULT NOW()
);

DROP TRIGGER IF EXISTS trg_feature_flags_updated_at ON feature_flags;
CREATE TRIGGER trg_feature_flags_updated_at
BEFORE UPDATE ON feature_flags
FOR EACH ROW EXECUTE FUNCTION set_updated_at();

CREATE OR REPLACE FUNCTION notify_flag_change()
RETURNS TRIGGER AS $$
BEGIN
    PERFORM pg_notify('flags_changed',
        COALESCE(NEW.key, OLD.key));
    RETURN COALESCE(NEW, OLD);
END;
$$ LANGUAGE plpgsql;

DROP TRIGGER IF EXISTS trg_feature_flags_notify ON feature_flags;
CREATE TRIGGER trg_feature_flags_notify
AFTER INSERT OR UPDATE OR DELETE ON feature_flags
FOR EACH ROW EXECUTE FUNCTION notify_flag_change();
