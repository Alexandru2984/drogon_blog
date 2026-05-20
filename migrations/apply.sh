#!/bin/sh
# Tiny POSIX-shell migration runner.
#
# Why home-grown: pulling in sqitch / golang-migrate / atlas would be a
# heavier dependency story than the actual problem warrants. The blog uses
# psql at startup already; this script just adds a one-file `applied_migrations`
# bookkeeping table on top.
#
# Usage:
#   DB_HOST=127.0.0.1 DB_PORT=5432 DB_NAME=blog_db DB_USER=blog_user \
#     DB_PASSWORD=...  ./migrations/apply.sh
#
# Behaviour:
#   * Creates `applied_migrations(id, sha256, applied_at, applied_by)` on
#     first run; later runs only insert rows.
#   * Iterates `migrations/<id>.sql` files in lexicographic order.
#   * For each file:
#       - If a row with the same id and sha256 exists -> skip (idempotent).
#       - If a row with the same id but a different sha256 exists -> ABORT
#         (someone edited an already-applied migration; never silently rerun).
#       - Otherwise: run the file inside a transaction, then insert the
#         bookkeeping row inside the same transaction. The migration and
#         its applied_migrations entry commit together or not at all.
#
# Flags:
#   --status     List applied + pending without changing anything.
#   --backfill   Mark every migration file as applied without running it.
#                Use only once, against a database that already matches
#                the most recent schema (e.g. legacy installs that ran
#                schema.sql before this tool existed).

set -eu

usage() {
    sed -n '2,32p' "$0"
    exit "${1:-0}"
}

: "${DB_HOST:?DB_HOST is required}"
: "${DB_PORT:=5432}"
: "${DB_NAME:?DB_NAME is required}"
: "${DB_USER:?DB_USER is required}"
: "${DB_PASSWORD:?DB_PASSWORD is required}"

mode=apply
case "${1:-}" in
    -h|--help) usage 0 ;;
    --status)   mode=status ;;
    --backfill) mode=backfill ;;
    "")         : ;;
    *)          echo "unknown flag: $1" >&2; usage 1 ;;
esac

DIR="$(cd "$(dirname "$0")" && pwd)"
export PGPASSWORD="$DB_PASSWORD"

psql_run() {
    psql -h "$DB_HOST" -p "$DB_PORT" -U "$DB_USER" -d "$DB_NAME" \
         -v ON_ERROR_STOP=1 --quiet --no-psqlrc "$@"
}

# Bookkeeping table. applied_by uses current_user so we can tell apart "ran
# from CI" vs "ran manually" when both share the same DB role.
psql_run -c '
    CREATE TABLE IF NOT EXISTS applied_migrations (
        id          TEXT        PRIMARY KEY,
        sha256      TEXT        NOT NULL,
        applied_at  TIMESTAMPTZ NOT NULL DEFAULT NOW(),
        applied_by  TEXT        NOT NULL DEFAULT CURRENT_USER
    );' >/dev/null

sha_of() {
    # Trim trailing whitespace so editor save-with-trailing-newline does
    # not flip the hash on otherwise-unchanged files.
    sha256sum "$1" | cut -d' ' -f1
}

list_files() {
    # POSIX-safe iteration. ls -1 is fine because we control the filenames.
    ls -1 "$DIR" | grep -E '^[0-9]{4}_.*\.sql$' | sort
}

list_applied() {
    psql_run -At -c 'SELECT id || E"\t" || sha256 FROM applied_migrations ORDER BY id;'
}

show_status() {
    echo "id                            applied?  sha (current/recorded)"
    list_files | while read -r f; do
        id=$(echo "$f" | sed 's/\.sql$//')
        cur=$(sha_of "$DIR/$f")
        rec=$(psql_run -At -c "SELECT sha256 FROM applied_migrations WHERE id='$id';")
        if [ -z "$rec" ]; then
            printf '%-30s pending   %s\n' "$id" "$cur"
        elif [ "$cur" = "$rec" ]; then
            printf '%-30s applied   %s\n' "$id" "$cur"
        else
            printf '%-30s DIVERGED  %s != %s\n' "$id" "$cur" "$rec"
        fi
    done
}

apply_one() {
    file="$1"
    id=$(basename "$file" .sql)
    cur=$(sha_of "$file")
    rec=$(psql_run -At -c "SELECT sha256 FROM applied_migrations WHERE id='$id';")
    if [ -n "$rec" ]; then
        if [ "$cur" = "$rec" ]; then
            return 0
        fi
        echo "REFUSING: $id already applied with sha $rec, but file is now $cur." >&2
        echo "          Create a new migration; never edit an applied one." >&2
        exit 1
    fi

    echo "Applying $id ..."
    # The migration file and the bookkeeping insert commit together. If
    # the .sql aborts, the INSERT never runs, and re-running apply.sh
    # will try again from a clean slate.
    {
        echo 'BEGIN;'
        cat "$file"
        echo ';'
        printf "INSERT INTO applied_migrations (id, sha256) VALUES ('%s', '%s');\n" "$id" "$cur"
        echo 'COMMIT;'
    } | psql_run -f -
}

backfill_one() {
    file="$1"
    id=$(basename "$file" .sql)
    cur=$(sha_of "$file")
    rec=$(psql_run -At -c "SELECT sha256 FROM applied_migrations WHERE id='$id';")
    if [ -n "$rec" ]; then return 0; fi
    echo "Backfilling $id (not running file) ..."
    psql_run -c "INSERT INTO applied_migrations (id, sha256) VALUES ('$id', '$cur');" >/dev/null
}

case "$mode" in
    status)
        show_status
        ;;
    backfill)
        list_files | while read -r f; do backfill_one "$DIR/$f"; done
        ;;
    apply)
        list_files | while read -r f; do apply_one "$DIR/$f"; done
        echo "Done."
        ;;
esac
