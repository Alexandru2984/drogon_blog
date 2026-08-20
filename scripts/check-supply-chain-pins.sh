#!/usr/bin/env bash
set -euo pipefail

# CI jobs execute these references with access to the checkout and a GitHub
# token. Keep every remote action/image immutable and make drift an explicit,
# reviewable dependency update instead of silently following a moved tag.
errors=0

if command -v rg >/dev/null 2>&1; then
    search_lines() { rg -n --no-heading "$@"; }
    search_quiet() { rg -q "$@"; }
    search_fixed_quiet() { rg -Fq "$@"; }
else
    search_lines() { grep -Ern "$@"; }
    search_quiet() { grep -Eq "$@"; }
    search_fixed_quiet() { grep -Fq "$@"; }
fi

report_floating() {
    printf 'floating supply-chain input: %s\n' "$1" >&2
    errors=$((errors + 1))
}

while IFS= read -r entry; do
    value=${entry#*uses:}
    value=${value%%#*}
    value=${value//[[:space:]]/}
    if [[ "$value" == ./* ]]; then
        continue
    fi
    if [[ ! "$value" =~ ^[^@]+@[0-9a-f]{40}$ ]]; then
        report_floating "$entry"
    fi
done < <(search_lines 'uses:[[:space:]]*[^[:space:]]+' .github/workflows)

while IFS= read -r entry; do
    value=${entry#*image:}
    value=${value%%#*}
    value=${value//[[:space:]]/}
    if [[ ! "$value" =~ ^[^@]+@sha256:[0-9a-f]{64}$ ]]; then
        report_floating "$entry"
    fi
done < <(search_lines '^[[:space:]]+image:[[:space:]]+[^[:space:]]+' \
    docker-compose.yml .github/workflows)

while IFS= read -r entry; do
    image=${entry#*FROM }
    image=${image%% *}
    if [[ "$image" != scratch && ! "$image" =~ @sha256:[0-9a-f]{64}$ ]]; then
        report_floating "$entry"
    fi
done < <(search_lines '^FROM[[:space:]]+' Dockerfile)

if ! head -n 1 Dockerfile \
    | search_quiet '^# syntax=[^@[:space:]]+@sha256:[0-9a-f]{64}$'; then
    report_floating 'Dockerfile frontend syntax image'
fi

# These images are invoked from shell steps, so they do not appear under an
# `image:` YAML key and need a separate regression guard.
while IFS= read -r entry; do
    report_floating "$entry"
done < <(search_lines \
    '(ghcr\.io/[^@[:space:]\\]+|stoplight/spectral:[^@[:space:]\\]+)([[:space:]\\]|$)' \
    .github/workflows || true)

# The command substitution and variable are literal Dockerfile text.
# shellcheck disable=SC2016
if ! search_quiet '^ARG DROGON_COMMIT=[0-9a-f]{40}$' Dockerfile \
    || ! search_fixed_quiet 'test "$(git -C drogon rev-parse HEAD)" = "${DROGON_COMMIT}"' Dockerfile; then
    report_floating 'Drogon source revision is not commit-verified'
fi

if ((errors > 0)); then
    printf '%d unpinned supply-chain input(s) found\n' "$errors" >&2
    exit 1
fi

printf 'All executable CI and container inputs are immutably pinned.\n'
