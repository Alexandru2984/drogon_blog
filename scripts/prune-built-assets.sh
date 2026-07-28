#!/usr/bin/env bash
# Delete built frontend chunks that public/index.html no longer references.
#
# Vite runs with emptyOutDir:false, because public/ is also where
# user-uploaded content is served from (public/uploads is a symlink) and
# wiping the directory would take the uploads with it. The cost is that
# every rebuild leaves the previous build's hashed chunks behind for ever —
# they accumulate in git and on disk, and nothing ever removes them.
#
# What "referenced" means here: index.html names the entry chunks, and
# those import the rest by hashed filename. So the reachable set is
# computed by following imports transitively from index.html rather than
# by reading index.html alone — a lazy route chunk is only ever named by
# another chunk.
#
# Usage:
#   scripts/prune-built-assets.sh           # report only
#   scripts/prune-built-assets.sh --delete  # actually remove

set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
assets="${root}/public/assets"
index="${root}/public/index.html"

[ -d "$assets" ] || { echo "no public/assets; nothing to do"; exit 0; }
[ -f "$index" ]  || { echo "error: public/index.html missing" >&2; exit 1; }

do_delete=0
[[ "${1:-}" == "--delete" ]] && do_delete=1

reachable="$(mktemp)"; queue="$(mktemp)"
trap 'rm -f "$reachable" "$queue"' EXIT

grep -oE 'assets/[A-Za-z0-9._-]+\.(js|css)' "$index" | sed 's|assets/||' | sort -u > "$queue"

# Transitive closure. Each pass adds files named by the ones already known;
# stop when a pass discovers nothing new.
: > "$reachable"
while [ -s "$queue" ]; do
    cat "$queue" >> "$reachable"
    next="$(mktemp)"
    while read -r f; do
        [ -f "$assets/$f" ] || continue
        grep -oE '[A-Za-z0-9._-]+\.(js|css)' "$assets/$f" 2>/dev/null || true
    done < "$queue" | sort -u > "$next"
    comm -23 "$next" <(sort -u "$reachable") > "$queue"
    rm -f "$next"
done
sort -u "$reachable" -o "$reachable"

total=0; orphans=0
for f in "$assets"/*; do
    [ -f "$f" ] || continue
    total=$((total + 1))
    base="$(basename "$f")"
    if ! grep -qxF "$base" "$reachable"; then
        orphans=$((orphans + 1))
        if [ "$do_delete" = "1" ]; then rm -f "$f"; else echo "  orphan: $base"; fi
    fi
done

if [ "$do_delete" = "1" ]; then
    echo "pruned $orphans orphaned asset(s); $((total - orphans)) still referenced"
else
    echo "$orphans of $total asset(s) are unreferenced (run with --delete to remove)"
fi
