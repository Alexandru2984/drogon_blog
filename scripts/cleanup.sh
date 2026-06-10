#!/bin/sh
# Drogon Blog — periodic maintenance.
#
# Two jobs that otherwise accumulate unbounded:
#   1. password_reset_tokens whose expires_at has passed. The request/reset
#      flow deletes a token on use, but tokens that are never redeemed linger
#      as whole rows forever. One-time email-verification tokens live as
#      columns on users and are cleared in place once expired.
#   2. Orphaned upload files. Avatars are replaced (the old jpg stays on disk)
#      and post images are embedded in markdown content (deleting a post leaves
#      its images behind). We delete only files older than a grace window AND
#      not referenced anywhere, so an upload mid-flow (saved to disk before its
#      DB row/content commits) is never reaped.
#
# Env vars:
#   DB_HOST/DB_PORT/DB_NAME/DB_USER/DB_PASSWORD  — read from .env in production
#   BLOG_CLEANUP_GRACE_DAYS  default 1 — only prune files older than this
#   DRY_RUN=1                report what would be deleted, change nothing
#
# Exit 0 on success; non-zero on any psql failure. Run via the
# drogon-blog-maintenance.timer; stderr is captured by journald.

set -eu

: "${DB_HOST:?DB_HOST is required}"
: "${DB_PORT:=5432}"
: "${DB_NAME:?DB_NAME is required}"
: "${DB_USER:?DB_USER is required}"
: "${DB_PASSWORD:?DB_PASSWORD is required}"

GRACE_DAYS="${BLOG_CLEANUP_GRACE_DAYS:-1}"
DRY_RUN="${DRY_RUN:-0}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
export PGPASSWORD="$DB_PASSWORD"

psql_run() {
    psql -h "$DB_HOST" -p "$DB_PORT" -U "$DB_USER" -d "$DB_NAME" \
         -v ON_ERROR_STOP=1 --quiet --no-psqlrc "$@"
}

# ---- 1. Expired tokens ----
if [ "$DRY_RUN" = "1" ]; then
    n=$(psql_run -At -c "SELECT count(*) FROM password_reset_tokens WHERE expires_at < NOW();")
    echo "[dry-run] would prune $n expired reset token(s)"
else
    n=$(psql_run -At -c "WITH d AS (DELETE FROM password_reset_tokens WHERE expires_at < NOW() RETURNING 1) SELECT count(*) FROM d;")
    echo "pruned $n expired reset token(s)"
    psql_run -c "UPDATE users SET email_verification_token = NULL, email_verification_expires = NULL WHERE email_verification_expires IS NOT NULL AND email_verification_expires < NOW();" >/dev/null
    echo "cleared expired email-verification tokens"
fi

# ---- 2. Orphaned uploads ----
# Collect the set of still-referenced filenames once (cheap, the tables are
# small) rather than querying per file. A disk file whose basename is not in
# the set, and older than the grace window, is an orphan.
refs="$(mktemp -d)"
trap 'rm -rf "$refs"' EXIT

# Avatars: users.profile_image holds the exact public path.
psql_run -At -c \
    "SELECT regexp_replace(profile_image, '^/uploads/profiles/', '') \
       FROM users WHERE profile_image LIKE '/uploads/profiles/%';" \
    > "$refs/profiles"

# Post images: embedded as markdown inside posts.content; pull every
# /uploads/posts/<file>.jpg reference out with a global regex.
psql_run -At -c \
    "SELECT DISTINCT (regexp_matches(content, '/uploads/posts/([A-Za-z0-9_.-]+\.jpg)', 'g'))[1] \
       FROM posts;" \
    > "$refs/posts"

prune_dir() {
    dir="$1"; reffile="$2"
    [ -d "$dir" ] || return 0
    find "$dir" -type f -name '*.jpg' -mtime "+$GRACE_DAYS" | while IFS= read -r f; do
        if grep -qxF "$(basename "$f")" "$reffile"; then continue; fi
        if [ "$DRY_RUN" = "1" ]; then
            echo "[dry-run] orphan: $f"
        else
            echo "removing orphan: $f"
            rm -f "$f"
        fi
    done
}

prune_dir "$ROOT/uploads/profiles" "$refs/profiles"
prune_dir "$ROOT/uploads/posts"    "$refs/posts"

echo "maintenance done"
