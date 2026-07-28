#!/usr/bin/env bash
#
# Regenerate ops/nginx/cloudflare-realip.conf from Cloudflare's published
# edge ranges.
#
# The output belongs at http level (/etc/nginx/conf.d/), not in the blog
# vhost: it is host-wide policy that every Cloudflare-fronted vhost on the
# box shares, and duplicating it per-server means two lists that drift.
#
# Why it has to stay current: the ranges are the trust boundary for
# `real_ip_header CF-Connecting-IP`. A range
# that Cloudflare adds after this file was generated arrives at the
# origin as an untrusted peer, so requests proxied through it get rate
# limited against the Cloudflare edge IP rather than the visitor's —
# every visitor behind that edge shares one bucket. Conversely a range
# Cloudflare *retires* and someone else later acquires would be trusted
# to declare arbitrary client IPs, which is exactly the spoof this whole
# mechanism exists to prevent. Refresh on a schedule, not on incident.
#
# Usage:
#   scripts/update-cloudflare-ips.sh          # rewrite the conf in place
#   scripts/update-cloudflare-ips.sh --check  # exit 1 if it is stale
#
# --check is what CI runs; it never writes.

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
out="${repo_root}/ops/nginx/cloudflare-realip.conf"

check_only=0
[[ "${1:-}" == "--check" ]] && check_only=1

# Cloudflare serves both lists WITHOUT a trailing newline. A plain
# `while read` drops the final line, which silently shrinks the trust list
# by one range per family — every visitor behind those edges would then be
# rate-limited against the edge address instead of their own. The
# `|| [ -n "$r" ]` clause processes that last unterminated line.
emit_ranges() {
    while read -r r || [ -n "$r" ]; do
        [ -n "$r" ] && echo "set_real_ip_from $r;"
    done < "$1"
}

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

for family in v4 v6; do
    if ! curl -fsS --max-time 30 "https://www.cloudflare.com/ips-${family}" \
         -o "${tmp}/${family}.txt"; then
        echo "error: could not fetch Cloudflare ips-${family}" >&2
        exit 2
    fi
    # A truncated or error response would silently shrink the trust list,
    # so demand something that at least looks like CIDR notation.
    if ! grep -qE '^[0-9a-fA-F:.]+/[0-9]+$' "${tmp}/${family}.txt"; then
        echo "error: ips-${family} did not look like a CIDR list" >&2
        exit 2
    fi
done

{
    echo "# Cloudflare edge IP ranges — set_real_ip_from directives."
    echo "#"
    echo "# GENERATED FILE. Do not edit by hand; run scripts/update-cloudflare-ips.sh"
    echo "# and commit the result. Sources:"
    echo "#   https://www.cloudflare.com/ips-v4"
    echo "#   https://www.cloudflare.com/ips-v6"
    echo "#"
    echo "# Last refreshed: $(date -u +%Y-%m-%d)"
    echo ""
    emit_ranges "${tmp}/v4.txt"
    echo ""
    emit_ranges "${tmp}/v6.txt"
    echo ""
    echo "# Rewrite \$remote_addr from CF-Connecting-IP, but only for peers in"
    echo "# the ranges above. A direct connection to the origin keeps its true"
    echo "# peer address no matter what headers it sends."
    echo "real_ip_header CF-Connecting-IP;"
    echo "# CF-Connecting-IP is a single address that Cloudflare overwrites,"
    echo "# not a chain, so there is no list to walk back through."
    echo "real_ip_recursive off;"
} > "${tmp}/cloudflare-realip.conf"

# Compare on the directives alone — the header carries a refresh date that
# changes on every run and would make --check permanently dirty.
strip_header() { grep '^set_real_ip_from' "$1" | sort; }

if [[ $check_only -eq 1 ]]; then
    if diff -q <(strip_header "$out") <(strip_header "${tmp}/cloudflare-realip.conf") \
         >/dev/null 2>&1; then
        echo "ops/nginx/cloudflare-realip.conf is up to date"
        exit 0
    fi
    echo "ops/nginx/cloudflare-realip.conf is STALE — run scripts/update-cloudflare-ips.sh" >&2
    diff <(strip_header "$out") <(strip_header "${tmp}/cloudflare-realip.conf") || true
    exit 1
fi

cp "${tmp}/cloudflare-realip.conf" "$out"
echo "wrote $out ($(grep -c '^set_real_ip_from' "$out") ranges)"
echo "deploy with:"
echo "  sudo cp $out /etc/nginx/conf.d/cloudflare-realip.conf"
echo "  sudo nginx -t && sudo systemctl reload nginx"
