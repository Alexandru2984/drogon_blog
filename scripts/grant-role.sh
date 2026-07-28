#!/bin/sh
# Grant or revoke a staff role.
#
# There is deliberately no HTTP endpoint for this. Promoting to admin is
# the one action with no safe failure mode — an endpoint for it is a
# permanent escalation target, and the operator who needs it already has
# shell and database access. Roles are therefore set out of band.
#
# Usage:
#   DB_HOST=… DB_NAME=… DB_USER=… DB_PASSWORD=… \
#     scripts/grant-role.sh <username> <user|moderator|admin>
#
# Reads the same DB_* variables as migrations/apply.sh.

set -eu

user="${1:-}"
role="${2:-}"

if [ -z "$user" ] || [ -z "$role" ]; then
    echo "usage: $0 <username> <user|moderator|admin>" >&2
    exit 2
fi
case "$role" in
    user|moderator|admin) ;;
    *) echo "error: role must be user, moderator or admin" >&2; exit 2 ;;
esac

PGPASSWORD="${DB_PASSWORD:-}" psql \
    -h "${DB_HOST:-127.0.0.1}" -p "${DB_PORT:-5432}" \
    -U "${DB_USER:-blog_user}" -d "${DB_NAME:-blog_db}" \
    -v ON_ERROR_STOP=1 --quiet --no-psqlrc -At \
    -c "UPDATE users SET role = '$role' WHERE username = '$user' RETURNING username || ' -> ' || role;" \
    | grep . || { echo "error: no such user '$user'" >&2; exit 1; }
