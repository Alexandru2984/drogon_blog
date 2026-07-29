#!/bin/sh
# Drogon Blog — periodic maintenance.
#
# Several tables and one directory that otherwise accumulate unbounded:
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

# ---- 1b. Retired session rows ----
# user_sessions keeps a row per login so the account page can show where the
# user is signed in. Revoked rows have no further use once they are older
# than the session lifetime: the sessions they describe could not be valid
# even if they had not been revoked, and every process rebuilds its
# in-memory revocation set from scratch on restart anyway.
#
# 30 days is comfortably past the 14-day session_timeout, which leaves the
# recent history readable if someone is investigating an incident.
if [ "$DRY_RUN" = "1" ]; then
    n=$(psql_run -At -c "SELECT count(*) FROM user_sessions WHERE revoked_at IS NOT NULL AND revoked_at < NOW() - INTERVAL '30 days';")
    echo "[dry-run] would prune $n retired session row(s)"
else
    n=$(psql_run -At -c "WITH d AS (DELETE FROM user_sessions WHERE revoked_at IS NOT NULL AND revoked_at < NOW() - INTERVAL '30 days' RETURNING 1) SELECT count(*) FROM d;")
    echo "pruned $n retired session row(s)"
fi

# ---- 1c. Day-old view dedup rows ----
# post_views exists only to stop one reader's refresh from counting twice
# within a day. The running total lives on posts.view_count, so a row whose
# day has passed carries no information — it is dead weight on a table that
# grows with every reader of every post.
#
# 7 days rather than 1 so a timezone-straddling deployment or a clock skew
# cannot delete a row that is still deduplicating.
if [ "$DRY_RUN" = "1" ]; then
    n=$(psql_run -At -c "SELECT count(*) FROM post_views WHERE viewed_on < CURRENT_DATE - 7;")
    echo "[dry-run] would prune $n stale view row(s)"
else
    n=$(psql_run -At -c "WITH d AS (DELETE FROM post_views WHERE viewed_on < CURRENT_DATE - 7 RETURNING 1) SELECT count(*) FROM d;")
    echo "pruned $n stale view row(s)"
fi

# ---- 1d. Tags no post carries ----
# post_tags cascades when a post is deleted, but the tag row itself stays.
# /tags already hides them (it joins through to a visible post), so this is
# not a correctness problem — it is an unbounded table of names nobody can
# reach, and it keeps a deleted post's tag squatting on its slug so a later
# author cannot claim the display spelling.
if [ "$DRY_RUN" = "1" ]; then
    n=$(psql_run -At -c "SELECT count(*) FROM tags t WHERE NOT EXISTS (SELECT 1 FROM post_tags pt WHERE pt.tag_id = t.id);")
    echo "[dry-run] would prune $n unused tag(s)"
else
    n=$(psql_run -At -c "WITH d AS (DELETE FROM tags t WHERE NOT EXISTS (SELECT 1 FROM post_tags pt WHERE pt.tag_id = t.id) RETURNING 1) SELECT count(*) FROM d;")
    echo "pruned $n unused tag(s)"
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
