-- Two-factor authentication: TOTP secrets, WebAuthn credentials, and
-- single-use recovery codes. All three tables are per-user, scoped via
-- a foreign key with ON DELETE CASCADE — purging the user purges every
-- piece of authenticator state without orphaned rows.
--
-- Threat model
--   * TOTP secrets are stored plaintext (industry standard: HMAC keys
--     need to be retrievable to regenerate codes). Compensating control:
--     this column never leaves the row to the wire after enrollment.
--   * Recovery codes are stored as Argon2id hashes, never plaintext. The
--     user sees each plaintext exactly once at issuance.
--   * WebAuthn public keys are by definition safe to store unencrypted;
--     possession of the matching private key sits on the authenticator
--     hardware and is never sent to us.

-- ---- TOTP enrollment state ----
-- One row per user. `enabled` flips to TRUE only after a successful
-- six-digit confirmation, so a half-finished enrollment (we sent the
-- secret to the client but they never confirmed) does NOT lock them
-- out of their account.
CREATE TABLE IF NOT EXISTS user_totp_secrets (
    user_id      INTEGER     PRIMARY KEY REFERENCES users(id) ON DELETE CASCADE,
    secret_b32   TEXT        NOT NULL,
    enabled      BOOLEAN     NOT NULL DEFAULT FALSE,
    created_at   TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    confirmed_at TIMESTAMPTZ
);

-- ---- WebAuthn credentials (passkeys) ----
-- credential_id is the binary credentialId returned by the authenticator,
-- base64url-encoded for portability. public_key is the raw COSE-encoded
-- public key (CBOR). sign_count is the monotonic counter the spec asks
-- us to compare against on every assertion — a regression hints at
-- a cloned authenticator and triggers a hard reject on the auth path.
CREATE TABLE IF NOT EXISTS user_webauthn_credentials (
    id              BIGINT      GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    user_id         INTEGER     NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    credential_id   TEXT        NOT NULL UNIQUE,
    public_key      BYTEA       NOT NULL,
    sign_count      BIGINT      NOT NULL DEFAULT 0,
    nickname        TEXT        NOT NULL DEFAULT '',
    created_at      TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    last_used_at    TIMESTAMPTZ
);
CREATE INDEX IF NOT EXISTS idx_webauthn_user
    ON user_webauthn_credentials(user_id);

-- ---- Recovery codes (one-time-use, Argon2id-hashed) ----
-- Issued in batches of 10 at TOTP / passkey enrollment. A consumed code
-- is kept in the table (with used_at set) for audit purposes — never
-- DELETEd, so a forensic question of "which recovery code unlocked
-- this account" still has an answer.
CREATE TABLE IF NOT EXISTS user_recovery_codes (
    id          BIGINT      GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    user_id     INTEGER     NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    code_hash   TEXT        NOT NULL,
    used_at     TIMESTAMPTZ,
    created_at  TIMESTAMPTZ NOT NULL DEFAULT NOW()
);
CREATE INDEX IF NOT EXISTS idx_recovery_user_unused
    ON user_recovery_codes(user_id) WHERE used_at IS NULL;
