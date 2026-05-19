#!/bin/sh
set -e

# Wait for PostgreSQL to be reachable before applying the schema and starting.
: "${DB_HOST:=db}"
: "${DB_PORT:=5432}"
: "${DB_NAME:?DB_NAME is required}"
: "${DB_USER:?DB_USER is required}"
: "${DB_PASSWORD:?DB_PASSWORD is required}"

echo "Waiting for postgres at ${DB_HOST}:${DB_PORT}..."
i=0
until PGPASSWORD="$DB_PASSWORD" psql -h "$DB_HOST" -p "$DB_PORT" -U "$DB_USER" -d "$DB_NAME" -c '\q' >/dev/null 2>&1; do
    i=$((i + 1))
    if [ "$i" -gt 60 ]; then
        echo "Postgres did not become ready in time" >&2
        exit 1
    fi
    sleep 1
done

echo "Applying schema..."
PGPASSWORD="$DB_PASSWORD" psql -h "$DB_HOST" -p "$DB_PORT" -U "$DB_USER" -d "$DB_NAME" \
    -v ON_ERROR_STOP=1 -f /app/schema.sql

echo "Starting Drogon blog..."
exec /app/blog
