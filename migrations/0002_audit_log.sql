-- Tamper-resistant log of sensitive actions. Insert-only at the app layer;
-- we deliberately do not expose UPDATE / DELETE handlers, and the actor_id
-- FK uses ON DELETE SET NULL so a user account deletion does not purge the
-- audit trail (the row keeps the historical actor_ip / metadata).
--
-- Columns
--   action      Dotted action key, e.g. 'login.ok', 'login.fail',
--               'password.reset', 'profile.email.change', 'post.delete'.
--               Stable so dashboards / SIEM rules can pivot on it.
--   target_kind / target_id   Optional pointer to the object the action
--                             affected ('post' / 42, 'user' / 7). Not a
--                             FK because target tables vary.
--   metadata    Free-form JSON for action-specific context (e.g. the
--               username that was attempted on a login.fail, or the
--               old/new email on a profile.email.change). PII-light by
--               convention: never log password material, tokens, or
--               session IDs into here.
--   req_id     Correlates with the structured access log entry that
--              recorded the same request, so SIEM/Loki can hop between
--              the two.
CREATE TABLE IF NOT EXISTS audit_log (
    id          BIGINT      GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    occurred_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    actor_id    INTEGER     REFERENCES users(id) ON DELETE SET NULL,
    actor_ip    TEXT        NOT NULL,
    action      TEXT        NOT NULL,
    target_kind TEXT,
    target_id   BIGINT,
    metadata    JSONB       NOT NULL DEFAULT '{}'::jsonb,
    req_id      TEXT
);

-- Per-actor history: "show me everything user 42 did, newest first".
CREATE INDEX IF NOT EXISTS idx_audit_log_actor_time
    ON audit_log(actor_id, occurred_at DESC);

-- Per-action rollup: "how many login.fail events in the last hour".
CREATE INDEX IF NOT EXISTS idx_audit_log_action_time
    ON audit_log(action, occurred_at DESC);
