#!/bin/sh
# Drogon Blog — PostgreSQL backup with rolling retention.
#
# Why pg_dump (logical) instead of WAL archiving / pg_basebackup:
#   * single-node, single-tenant DB; no replicas to feed from a base
#     backup;
#   * RPO target is "yesterday's data" — losing up to 24h on the worst
#     day is acceptable for a personal blog and dramatically simpler
#     than a streaming-replication setup;
#   * --format=custom (`-Fc`) is compressed (zlib by default), supports
#     selective restore via pg_restore, and is the format we restore
#     against in scripts/restore.sh + the CI roundtrip job.
#
# Why home-grown rotation instead of `pg_back` / `barman`:
#   * single file, no extra dependency, easy to audit. The retention
#     math is short: every backup is named with an ISO date stamp, the
#     script keeps the last N daily files, then a single Sunday file
#     per kept week, then a single first-of-month file per kept month.
#     Anything outside those three windows is deleted.
#
# Env vars:
#   DB_HOST          (required) — read from .env in production
#   DB_PORT          default 5432
#   DB_NAME          (required)
#   DB_USER          (required)
#   DB_PASSWORD      (required) — exported as PGPASSWORD locally
#   BLOG_BACKUP_DIR  default /var/backups/drogon-blog
#   BLOG_BACKUP_KEEP_DAILY    default 7
#   BLOG_BACKUP_KEEP_WEEKLY   default 4
#   BLOG_BACKUP_KEEP_MONTHLY  default 6
#
# Exit codes:
#   0 on success; non-zero on any pg_dump / fs failure. The timer's
#   journal capture preserves stderr so failures are visible via
#   `journalctl -u drogon-blog-backup.service`.

set -eu

: "${DB_HOST:?DB_HOST is required}"
: "${DB_PORT:=5432}"
: "${DB_NAME:?DB_NAME is required}"
: "${DB_USER:?DB_USER is required}"
: "${DB_PASSWORD:?DB_PASSWORD is required}"
: "${BLOG_BACKUP_DIR:=/var/backups/drogon-blog}"
: "${BLOG_BACKUP_KEEP_DAILY:=7}"
: "${BLOG_BACKUP_KEEP_WEEKLY:=4}"
: "${BLOG_BACKUP_KEEP_MONTHLY:=6}"

stamp=$(date -u '+%Y-%m-%dT%H-%M-%SZ')
file="$BLOG_BACKUP_DIR/blog-$stamp.dump"

mkdir -p "$BLOG_BACKUP_DIR"
# 700 on the directory keeps the dumps off other-readable even on a
# multi-tenant VPS; pg_dump's output contains every row in the DB.
chmod 700 "$BLOG_BACKUP_DIR"

echo "[backup] dumping $DB_NAME to $file"

export PGPASSWORD="$DB_PASSWORD"
# --no-owner / --no-privileges: restore should slot into any role
# layout, not insist on the exact roles from this DB. The application
# only needs DML access on its own schema, restored separately by the
# grants in restore.sh.
# --compress=9: trades CPU for bytes; backups run off-peak so the CPU
# cost doesn't matter, and the network/disk savings do on long retention.
pg_dump \
    --host="$DB_HOST" --port="$DB_PORT" \
    --username="$DB_USER" --dbname="$DB_NAME" \
    --format=custom \
    --compress=9 \
    --no-owner --no-privileges \
    --file="$file.partial"

# Atomic publish: only rename the file once pg_dump exited 0. A
# half-written `.partial` left behind on crash is easy to spot and
# excluded from retention scans below.
mv "$file.partial" "$file"
chmod 600 "$file"
echo "[backup] wrote $(du -h "$file" | awk '{print $1}') to $file"

# -----------------------------------------------------------------------
# Retention.
#
# Phase 1: keep the most-recent N daily dumps.
# Phase 2: of the older dumps, keep one Sunday file per week, up to N weeks.
# Phase 3: of dumps older than the weekly window, keep one file dated
#          on-or-after the 1st of the month, up to N months.
# Phase 4: delete every dump file not flagged "keep" above.
#
# Implementation uses `find -mtime` instead of parsing the date out of
# the filename so the keep windows are tied to actual mtime — keeps the
# script robust against system clock corrections.
# -----------------------------------------------------------------------

# Quiet `set -u` on the keep-flag file we create below.
keep_list=$(mktemp)
trap 'rm -f "$keep_list"' EXIT

cd "$BLOG_BACKUP_DIR"

# All complete dumps, newest first.
all_dumps=$(ls -1t blog-*.dump 2>/dev/null || true)
[ -z "$all_dumps" ] && exit 0

# Phase 1: most recent BLOG_BACKUP_KEEP_DAILY entries.
echo "$all_dumps" | head -n "$BLOG_BACKUP_KEEP_DAILY" >> "$keep_list"

# Phase 2: skip the daily window, then take one Sunday file per week.
# We iterate the rest, recording the first dump file whose date lands
# on a Sunday for each ISO week we encounter.
weekly_taken=0
last_week=""
echo "$all_dumps" | tail -n +"$((BLOG_BACKUP_KEEP_DAILY + 1))" | while read -r f; do
    [ -z "$f" ] && continue
    # Extract the YYYY-MM-DD prefix from blog-YYYY-MM-DDTHH-MM-SSZ.dump
    d="${f#blog-}"
    d="${d%%T*}"
    week=$(date -d "$d" '+%G-%V' 2>/dev/null || echo "")
    dow=$(date -d "$d" '+%u' 2>/dev/null || echo "0")  # 1=Mon..7=Sun
    if [ "$dow" = "7" ] && [ "$week" != "$last_week" ] && [ "$weekly_taken" -lt "$BLOG_BACKUP_KEEP_WEEKLY" ]; then
        echo "$f" >> "$keep_list"
        last_week="$week"
        weekly_taken=$((weekly_taken + 1))
    fi
done

# Phase 3: monthly — first day-of-month file we see, walking from newest.
monthly_taken=0
last_month=""
echo "$all_dumps" | tail -n +"$((BLOG_BACKUP_KEEP_DAILY + 1))" | while read -r f; do
    [ -z "$f" ] && continue
    d="${f#blog-}"
    d="${d%%T*}"
    month=$(date -d "$d" '+%Y-%m' 2>/dev/null || echo "")
    dom=$(date -d "$d" '+%-d' 2>/dev/null || echo "99")
    if [ "$dom" -le "3" ] && [ "$month" != "$last_month" ] && [ "$monthly_taken" -lt "$BLOG_BACKUP_KEEP_MONTHLY" ]; then
        echo "$f" >> "$keep_list"
        last_month="$month"
        monthly_taken=$((monthly_taken + 1))
    fi
done

# Phase 4: delete dumps not in the keep_list.
sort -u "$keep_list" > "$keep_list.sorted"
deleted=0
for f in $all_dumps; do
    if ! grep -qx "$f" "$keep_list.sorted"; then
        rm -f "$f"
        deleted=$((deleted + 1))
    fi
done
# Also sweep stale .partial files older than 1 day — interrupted runs.
find . -maxdepth 1 -name 'blog-*.dump.partial' -mtime +1 -delete 2>/dev/null || true

kept=$(wc -l < "$keep_list.sorted")
rm -f "$keep_list.sorted"
echo "[backup] retention: kept=$kept deleted=$deleted"
