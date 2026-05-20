#!/usr/bin/env python3
"""Walk a results/ run directory and emit a markdown table summarising every
scenario. Pulls the trend percentiles k6 records under http_req_duration.
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

# k6's summary JSON nests trend stats deep; some k6 versions store them as
# {p(95): value} directly, others as {percentiles: {95: value}}. Probe both.
TREND_KEYS = ('p(50)', 'med', 'p(95)', 'p(99)', 'avg', 'max')

LABELS = {
    'feed_read':    'GET /posts (feed, JOIN authors)',
    'post_view':    'GET /posts/{id}',
    'search':       'GET /posts/search?q=… (FTS + ts_rank)',
    'auth_me_warm': 'GET /auth/me (warm session)',
}


def num(v, default=float('nan')):
    try: return float(v)
    except Exception: return default


def stat(metrics: dict, key: str) -> float:
    m = metrics.get('http_req_duration', {})
    # k6 v0.42+: {values: {avg, p(95), …}}
    # k6 v2.x:   {avg, p(95), …} at the top level
    if 'values' in m:
        return num(m['values'].get(key))
    return num(m.get(key))


def reqs(metrics: dict) -> float:
    m = metrics.get('http_reqs', {})
    if 'values' in m: return num(m['values'].get('rate'))
    return num(m.get('rate'))


def fail_rate(metrics: dict) -> float:
    m = metrics.get('http_req_failed', {})
    if 'values' in m: return num(m['values'].get('rate'))
    # k6 v2.x stores the current failure rate in `value`.
    if 'value' in m:  return num(m.get('value'))
    return num(m.get('rate'))


def main(argv: list[str]) -> int:
    if len(argv) != 2:
        print('usage: summarize.py <results-dir>', file=sys.stderr)
        return 2

    root = Path(argv[1])
    rows: list[tuple[str, dict]] = []
    for p in sorted(root.glob('*.json')):
        try:
            data = json.loads(p.read_text())
        except Exception:
            continue
        if 'metrics' not in data: continue
        rows.append((p.stem, data['metrics']))

    print('| Scenario | RPS | avg | p50 | p95 | p99 | max | error rate |')
    print('|---|---:|---:|---:|---:|---:|---:|---:|')
    for name, m in rows:
        label = LABELS.get(name, name)
        rps   = reqs(m)
        avg   = stat(m, 'avg')
        p50_a = stat(m, 'p(50)')
        p50   = p50_a if p50_a == p50_a else stat(m, 'med')  # NaN check
        p95   = stat(m, 'p(95)')
        p99   = stat(m, 'p(99)')
        mx    = stat(m, 'max')
        err   = fail_rate(m)
        print(f'| {label} | {rps:.0f} | {avg:.1f} ms | {p50:.1f} ms | '
              f'{p95:.1f} ms | {p99:.1f} ms | {mx:.1f} ms | {err*100:.2f}% |')
    return 0


if __name__ == '__main__':
    raise SystemExit(main(sys.argv))
