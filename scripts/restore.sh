#!/bin/sh
# Drogon Blog — PostgreSQL restore from pg_dump --format=custom.
#
# Workflow:
#   1. Validate the dump file exists and is readable.
#   2. Refuse to clobber a non-empty DB unless --force is given. The
#      check looks for any user table in the public schema; an empty
#      DB or "only applied_migrations" both count as restorable.
#   3. pg_restore with --clean --if-exists so a re-runnable restore
#      drops objects from the previous schema before recreating them.
#   4. Re-grant table/sequence privileges to the app role (DB_USER)
#      since the dump was taken with --no-privileges.
#   5. Verify applied_migrations + a couple of core tables exist so
#      the restore actually produced a working schema.
#
# Env vars (same shape as scripts/backup.sh):
#   DB_HOST DB_PORT DB_NAME DB_USER DB_PASSWORD
#
# Usage:
#   scripts/restore.sh path/to/blog-YYYY-MM-DDTHH-MM-SSZ.dump [--force]

set -eu

usage() {
    echo "usage: $0 <dump-file> [--force]" >&2
    exit 1
}

[ $# -ge 1 ] || usage
dump="$1"; shift || true
force=0
while [ $# -gt 0 ]; do
    case "$1" in
        --force) force=1 ;;
        *) usage ;;
    esac
    shift
done

[ -f "$dump" ] || { echo "dump file not found: $dump" >&2; exit 1; }
[ -r "$dump" ] || { echo "dump file not readable: $dump" >&2; exit 1; }

: "${DB_HOST:?DB_HOST is required}"
: "${DB_PORT:=5432}"
: "${DB_NAME:?DB_NAME is required}"
: "${DB_USER:?DB_USER is required}"
: "${DB_PASSWORD:?DB_PASSWORD is required}"

export PGPASSWORD="$DB_PASSWORD"

psql_q() {
    psql -h "$DB_HOST" -p "$DB_PORT" -U "$DB_USER" -d "$DB_NAME" \
         -v ON_ERROR_STOP=1 -At --quiet --no-psqlrc "$@"
}

# Pre-flight: count user tables. "0 tables" or "only applied_migrations"
# both mean it's safe to restore without --force.
existing=$(psql_q -c "
    SELECT COALESCE(string_agg(tablename, ','), '')
    FROM pg_tables
    WHERE schemaname = 'public'
      AND tablename <> 'applied_migrations';
")
if [ -n "$existing" ] && [ "$force" -ne 1 ]; then
    echo "REFUSING: target DB '$DB_NAME' already has tables: $existing" >&2
    echo "          Re-run with --force to drop & restore over them." >&2
    exit 1
fi

echo "[restore] restoring $dump into $DB_NAME"

# --clean --if-exists: each CREATE in the dump is preceded by a matching
# DROP ... IF EXISTS; lets us rerun restore over a partially-populated
# DB without manual cleanup. --no-owner skips ALTER OWNER (we restored
# with --no-owner, but a defensive flag costs nothing).
# --exit-on-error stops on the first failure instead of pretending the
# restore worked while half the tables are missing.
pg_restore \
    --host="$DB_HOST" --port="$DB_PORT" \
    --username="$DB_USER" --dbname="$DB_NAME" \
    --clean --if-exists \
    --no-owner --no-privileges \
    --exit-on-error \
    --single-transaction \
    "$dump"

# Re-grant: the dump was taken --no-privileges so the restored objects
# are owned by whoever ran pg_restore. The app role (DB_USER, same
# identity that runs this script in CI / on prod) already has DML on
# anything it created, so this is a no-op when DB_USER == owner — but
# explicit makes future "restore into a different DB" work without
# surprises.
psql_q -c "
    GRANT USAGE ON SCHEMA public TO \"$DB_USER\";
    GRANT SELECT, INSERT, UPDATE, DELETE ON ALL TABLES IN SCHEMA public TO \"$DB_USER\";
    GRANT USAGE, SELECT, UPDATE ON ALL SEQUENCES IN SCHEMA public TO \"$DB_USER\";
" > /dev/null

# Smoke: verify the schema landed.
applied=$(psql_q -c "SELECT count(*) FROM applied_migrations;")
users=$(psql_q -c "SELECT count(*) FROM users;")
posts=$(psql_q -c "SELECT count(*) FROM posts;")
echo "[restore] applied_migrations=$applied users=$users posts=$posts"

if [ "$applied" = "0" ]; then
    echo "WARNING: applied_migrations is empty — restored dump may predate the migration runner." >&2
fi
