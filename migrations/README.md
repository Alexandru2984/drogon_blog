# Migrations

Versioned, forward-only SQL migrations applied with a tiny POSIX shell
runner (`apply.sh`). No external dependency: just `psql` + `sha256sum`.

## Layout

```
migrations/
  0001_init.sql        Initial schema (everything that used to live in
                       schema.sql at the repo root).
  0002_audit_log.sql   Sensitive-action audit log.
  apply.sh             Runner — see below.
  README.md            This file.
```

## Naming

`NNNN_<slug>.sql`, four-digit prefix, lexicographic ordering. Never
renumber and never edit an applied migration — the runner refuses to
re-apply a file whose sha256 has drifted from what was recorded.

## Usage

```bash
# Apply all pending migrations against the database identified by env:
DB_HOST=127.0.0.1 DB_PORT=5432 \
DB_NAME=blog_db DB_USER=blog_user DB_PASSWORD=... \
  ./migrations/apply.sh

# See what is applied vs pending, without changing anything:
./migrations/apply.sh --status

# Mark every existing file as applied without actually running it.
# Use exactly once, on a database that was bootstrapped before the
# migrations tool existed (e.g. legacy installs that ran schema.sql):
./migrations/apply.sh --backfill
```

## Guarantees

- The migration file and the row in `applied_migrations` commit
  together inside one transaction. Either both land or neither does.
- Re-applying with the same file is a no-op (sha256 match).
- Re-applying with an edited file is a hard error: the runner prints
  the offending id and exits non-zero. The recipe in that case is to
  add a new migration, not to edit an old one.

## CI and test bootstrap

The CI workflow (`.github/workflows/ci.yml`) applies all migrations
before running the C++ integration tests. Tests therefore exercise
the same SQL path as production.
