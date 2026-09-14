#!/usr/bin/env bash
# Deploy drogon-blog on the host it runs from: pull, build, restart, verify.
#
# Called by the GitHub push webhook (hooks.json, id "drogon-blog") with this
# repository as the working directory. The generic cicd/deploy.sh it replaces
# only pulled and restarted, so a C++ change pushed to main was never built:
# the restart brought back the old binary and the log still said "Deploy OK".
#
#   1. git pull --ff-only — never makes a merge commit in the production
#      checkout; a diverged or conflicting tree stops the deploy instead.
#   2. cmake --build — incremental, so a no-op when no C++ changed. A failed
#      build stops here with the old process still serving. (public/ is
#      committed build output, so the pull has already made it live.)
#   3. restart, then wait for /readyz. If the new process is not ready in
#      time, the previous binary is put back and started again.
#
# Migrations are not applied: they stay a deliberate, manual step
# (migrations/apply.sh). A pull that brings new ones is flagged in the log.
#
# Environment:
#   DEPLOY_LOG        also append everything to this file
#   HEALTH_URL        readiness probe, default http://127.0.0.1:8092/readyz
#   HEALTH_TIMEOUT    seconds to wait for it, default 45
#   DEPLOY_DRY_RUN=1  skip the pull, the restarts and the binary swap; print
#                     them instead (the build and the probe still run)

set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/.."

SERVICE=drogon-blog.service
BIN=build/blog
PREV=build/blog.prev
HEALTH_URL=${HEALTH_URL:-http://127.0.0.1:8092/readyz}
HEALTH_TIMEOUT=${HEALTH_TIMEOUT:-45}
DRY=${DEPLOY_DRY_RUN:-0}

if [[ -n "${DEPLOY_LOG:-}" ]]; then
    mkdir -p "$(dirname "$DEPLOY_LOG")"
    exec > >(tee -a "$DEPLOY_LOG") 2>&1
fi

log() { printf '[%s] %s\n' "$(date '+%F %T %Z')" "$*"; }
run() { if [[ "$DRY" == 1 ]]; then log "dry-run: $*"; else "$@"; fi; }

ready() {
    local i
    for (( i = 0; i < HEALTH_TIMEOUT; i++ )); do
        curl -fs -o /dev/null --max-time 2 "$HEALTH_URL" && return 0
        sleep 1
    done
    return 1
}

# Two pushes in quick succession fire two webhooks. The second waits for the
# first instead of building and restarting on top of it.
exec 9>build/.deploy.lock
flock -w 900 9 || { log "❌ another deploy still holds the lock"; exit 1; }

log "🚀 Deploy start: drogon-blog"

before=$(git rev-parse HEAD)
if ! run git pull --ff-only origin main; then
    log "❌ git pull failed (diverged or conflicting tree) — nothing deployed"
    exit 1
fi
after=$(git rev-parse HEAD)
log "HEAD ${before:0:7} -> ${after:0:7}"

if [[ "$before" != "$after" ]] && ! git diff --quiet "$before" "$after" -- migrations/; then
    log "⚠️  new migrations — apply them by hand (migrations/apply.sh) if the code needs them:"
    git diff --name-only "$before" "$after" -- migrations/ | sed 's/^/      /'
fi

# Keep the image that is serving right now, so a failed start can be undone.
# Copied from /proc rather than from build/: if someone rebuilt without
# restarting, build/blog is no longer what is running, and "rolling back" to
# it would restore the wrong thing. /proc/PID/exe stays readable even after
# the file behind it has been replaced.
pid=$(systemctl show "$SERVICE" -p MainPID --value 2>/dev/null || echo 0)
if [[ "$pid" =~ ^[1-9][0-9]*$ ]] && cp "/proc/$pid/exe" "$PREV" 2>/dev/null; then
    chmod 755 "$PREV"
elif [[ -x "$BIN" ]]; then
    cp -p "$BIN" "$PREV"
fi

if ! cmake --build build --target blog -j"$(nproc)"; then
    log "❌ build failed — the running server was left alone"
    exit 1
fi
if [[ -f "$PREV" ]] && cmp -s "$BIN" "$PREV"; then
    log "binary is the one already running"
else
    log "new binary built"
fi

run sudo -n systemctl restart "$SERVICE"
if ready; then
    log "✅ Deploy OK: drogon-blog (${after:0:7})"
    exit 0
fi

log "❌ not ready after ${HEALTH_TIMEOUT}s — restoring the previous binary"
if [[ ! -f "$PREV" ]]; then
    log "❌ no previous binary to restore — check: journalctl -u $SERVICE"
    exit 1
fi
# Copy then rename: the failed binary may still be executing, and an
# executing file cannot be opened for writing (ETXTBSY). A rename replaces
# the directory entry without touching the running image.
run cp -p "$PREV" "$BIN.rollback"
run mv -f "$BIN.rollback" "$BIN"
run sudo -n systemctl restart "$SERVICE"
if ready; then
    log "❌ Deploy failed: rolled back to the previous binary (${after:0:7} is checked out but not running)"
else
    log "❌ Deploy failed: still not ready after rollback — check: journalctl -u $SERVICE"
fi
exit 1
