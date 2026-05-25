# Backup & restore runbook

PostgreSQL is the only stateful piece of the blog (uploaded images on
disk are an aux concern covered separately). This runbook is the
single source of truth for backup posture and disaster recovery.

## Posture

| Setting                  | Value                                          |
|--------------------------|------------------------------------------------|
| **Backup type**          | `pg_dump --format=custom` (logical, compressed) |
| **Schedule**             | `OnCalendar=*-*-* 02:30:00 UTC` daily          |
| **Persistent**           | `true` — missed runs catch up at next boot     |
| **Destination (prod)**   | `/var/lib/drogon-blog/backups/` (0700, owner `micu`) |
| **File name**            | `blog-YYYY-MM-DDTHH-MM-SSZ.dump` (UTC stamp)   |
| **Retention**            | 7 daily · 4 weekly (Sunday) · 6 monthly        |
| **RPO**                  | ≤ 24 h (worst case, last backup is yesterday)  |
| **RTO**                  | ≤ 5 min on the same VPS (`scripts/restore.sh`) |
| **CI coverage**          | `backup-restore` job in `.github/workflows/ci.yml` exercises backup → wipe → restore → row-level verification on every push |

The `pg_dump` is logical because the blog is a single-node setup with
no replicas to seed from a base backup, and the per-day RPO is
acceptable for a personal-blog workload. Custom format keeps the
output compressed (zlib `-Z 9`) and lets `pg_restore` rebuild
selectively.

## Operations

### Inspect the timer

```bash
sudo systemctl list-timers drogon-blog-backup.timer
sudo journalctl -u drogon-blog-backup.service -n 20
sudo ls -la /var/lib/drogon-blog/backups/
```

### Trigger an ad-hoc backup

```bash
sudo systemctl start drogon-blog-backup.service
sudo journalctl -u drogon-blog-backup.service -f
```

The service is `Type=oneshot`, so `start` runs synchronously and
exits with the script's status code.

### Restore (same VPS)

```bash
cd /home/micu/drogon_blog
set -a; . ./.env; set +a
DUMP=/var/lib/drogon-blog/backups/blog-<stamp>.dump

# Step 1: stop the app so it doesn't write while we restore.
sudo systemctl stop drogon-blog

# Step 2: drop & recreate the public schema. restore.sh refuses to
#         clobber a non-empty DB without --force; this is the safer
#         and more explicit reset path.
PGPASSWORD="$DB_PASSWORD" psql -h "$DB_HOST" -U "$DB_USER" -d "$DB_NAME" \
    -c 'DROP SCHEMA public CASCADE; CREATE SCHEMA public;'

# Step 3: restore.
./scripts/restore.sh "$DUMP"

# Step 4: start the app + smoke.
sudo systemctl start drogon-blog
curl -fsS http://127.0.0.1:8092/healthz
```

### Restore (fresh VPS)

1. Provision Postgres 17 with role `blog_user` and DB `blog_db`
   (whatever names the new `.env` carries).
2. Clone the repo, copy the dump file across (`scp …`), copy the
   `.env`.
3. Run `migrations/apply.sh` to seat the `applied_migrations` table —
   the restore will re-apply the actual schema, but the bookkeeping
   table needs to exist first.
4. Run `scripts/restore.sh /path/to/dump`.
5. Build the app (`cmake -B build && cmake --build build`),
   install + enable `systemd/drogon-blog.service` and
   `systemd/drogon-blog-backup.{service,timer}`.
6. Smoke: `curl http://127.0.0.1:8092/healthz`, log in as a real user.

## Verification

The CI job `backup-restore` runs on every push:

1. Spins up a fresh Postgres.
2. Applies migrations + seeds known rows (`alice`, `bob`, one post,
   one comment).
3. Runs `scripts/backup.sh`.
4. Drops the schema clean.
5. Runs `scripts/restore.sh`.
6. Asserts row counts and that `alice@example.test` survives the
   round-trip exactly.

If the job fails: the dump format or the restore code path
regressed — fix before merging, because the production timer would
otherwise produce dumps we couldn't restore.

## What's NOT in scope

- **Off-site copy.** Backups live on the same VPS as the DB; a
  full-host loss takes them with it. Adding an `rclone` sweep to S3 /
  B2 is the obvious next step but lives outside this runbook.
- **Continuous archiving (WAL).** Would lower RPO from 24 h to
  seconds at the cost of a streaming-archive setup we don't need
  for a personal blog.
- **Application-level state on disk** (uploaded profile images under
  `models/uploads/`). Mirror that directory separately when off-site
  backup is added — it's not part of the SQL dump.
