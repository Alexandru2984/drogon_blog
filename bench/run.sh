#!/usr/bin/env bash
# Drives the bench: seed data, warm the JIT, run each scenario, write a
# results matrix in markdown to ./results/<timestamp>/.
#
# Assumes the blog is reachable at $BASE_URL (default 127.0.0.1:8092) and
# that rate limiting is disabled or wide enough for the chosen --vus.
# The bench reads from / mutates the production DB unless the caller points
# BASE_URL at a separate compose stack.

set -euo pipefail

cd "$(dirname "$0")"

BASE_URL="${BASE_URL:-http://127.0.0.1:8092}"
DURATION="${DURATION:-30s}"
VUS="${VUS:-30}"
OUT_ROOT="results/$(date -u +%Y%m%dT%H%M%SZ)"
mkdir -p "$OUT_ROOT"

echo "== seeding =="
python3 seed.py --base "$BASE_URL"

echo "== warm-up =="
for _ in $(seq 1 100); do
    curl -s -o /dev/null "$BASE_URL/posts"             || true
    curl -s -o /dev/null "$BASE_URL/posts/search?q=v"  || true
done

run_scenario() {
    local name="$1" vus="$2" duration="$3"
    echo "== scenario: $name (vus=$vus, duration=$duration) =="
    k6 run \
        --vus "$vus" --duration "$duration" \
        --summary-export "$OUT_ROOT/${name}.json" \
        --tag "scenario=${name}" \
        -e "SCENARIO=${name}" \
        -e "BASE_URL=${BASE_URL}" \
        scenarios.js | tee "$OUT_ROOT/${name}.log"
}

run_scenario feed_read    "$VUS" "$DURATION"
run_scenario post_view    "$VUS" "$DURATION"
run_scenario search       "$VUS" "$DURATION"
run_scenario auth_me_warm "$VUS" "$DURATION"

python3 summarize.py "$OUT_ROOT" > "$OUT_ROOT/summary.md"
echo
echo "== summary =="
cat "$OUT_ROOT/summary.md"
echo
echo "results in $OUT_ROOT/"
