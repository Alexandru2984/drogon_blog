#!/usr/bin/env python3
"""Seed the blog with users + posts so the k6 scenarios have a corpus.

Idempotent: re-running deletes the previous bench users (and their posts via
ON DELETE CASCADE) before re-creating them, so seed counts stay deterministic
across runs.

Writes ./seed.json next to itself with the post IDs, search terms, and a
list of session cookies harvested from /auth/login so the auth_me_warm
scenario can hit /auth/me with a fresh-looking client.
"""

from __future__ import annotations

import argparse
import json
import os
import secrets
import sys
import urllib.error
import urllib.request
from http.cookies import SimpleCookie
from pathlib import Path

DEFAULT_BASE = os.environ.get('BASE_URL', 'http://127.0.0.1:8092')

# Mix of titles so /posts/search?q=… finds varied results, and so different
# weights (title vs content) get exercised.
POSTS = [
    ('postgresql tsvector primer',
     'Body discusses GIN indexes and how tsvector stores lexemes.'),
    ('drogon performance tuning',
     'Why N+1 queries hurt and how a JOIN crushes them.'),
    ('vue 3 composition api',
     'Reactive refs, computed values, watchers — composition over options.'),
    ('argon2id in practice',
     'Memory-hard hashing tuned for interactive logins.'),
    ('websockets and presence',
     'A real-time hub keyed by user id, fanning out new chat events.'),
]

NUM_USERS = 5

# Generated per run rather than hard-coded.
#
# The literal that used to sit here was the only secret gitleaks found in the
# whole history, and while it opens nothing today the reason is luck: this
# script registers real accounts through the real /auth/register, so the
# password is only harmless for as long as nobody points it at an instance
# that matters. Bench runs are driven against production hosts often enough
# that "nobody would" is not a control. A fresh random value per run also
# removes the temptation to reuse it by hand.
#
# BENCH_PASSWORD overrides it, for the case where a run has to be resumed and
# the existing users logged into again.
PASSWORD = os.environ.get('BENCH_PASSWORD') or 'bench-' + secrets.token_urlsafe(24)


def request_json(method: str, url: str, *, body=None, cookies=None, extra_headers=None):
    data = json.dumps(body).encode() if body is not None else None
    headers = {'Content-Type': 'application/json'}
    if cookies:
        headers['Cookie'] = '; '.join(f'{k}={v}' for k, v in cookies.items())
    if extra_headers:
        headers.update(extra_headers)
    req = urllib.request.Request(url, data=data, headers=headers, method=method)
    try:
        with urllib.request.urlopen(req, timeout=10) as r:
            payload = r.read()
            raw_cookies = r.headers.get_all('Set-Cookie') or []
            return r.status, payload, raw_cookies
    except urllib.error.HTTPError as e:
        return e.code, e.read(), e.headers.get_all('Set-Cookie') or []


def parse_set_cookies(set_cookie_lines: list[str]) -> dict[str, str]:
    out: dict[str, str] = {}
    for line in set_cookie_lines:
        sc = SimpleCookie()
        sc.load(line)
        for k, m in sc.items():
            out[k] = m.value
    return out


def login(base: str, username: str, password: str) -> dict[str, str]:
    code, _, set_cookies = request_json('POST', f'{base}/auth/login',
        body={'username': username, 'password': password})
    if code != 200:
        raise SystemExit(f'login failed for {username}: status {code}')
    return parse_set_cookies(set_cookies)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--base', default=DEFAULT_BASE)
    ap.add_argument('--out',  default=str(Path(__file__).parent / 'seed.json'))
    args = ap.parse_args()

    base = args.base.rstrip('/')

    # 1. Best-effort cleanup of any previous bench users so post IDs are
    # predictable. Done via SQL because there's no admin REST endpoint.
    try:
        import subprocess
        env_file = Path(__file__).resolve().parent.parent / '.env'
        env_vars = {}
        for raw in env_file.read_text().splitlines():
            if not raw or raw.startswith('#') or '=' not in raw: continue
            k, v = raw.split('=', 1)
            env_vars[k] = v
        pgpass = env_vars.get('DB_PASSWORD', '')
        subprocess.run(
            ['psql', '-h', env_vars.get('DB_HOST', '127.0.0.1'),
             '-U', env_vars.get('DB_USER', 'blog_user'),
             '-d', env_vars.get('DB_NAME', 'blog_db'),
             '-v', 'ON_ERROR_STOP=1',
             '-c', "DELETE FROM users WHERE username LIKE 'bench_%';"],
            env={**os.environ, 'PGPASSWORD': pgpass},
            check=True, capture_output=True)
    except Exception as e:
        print(f'  warn: pre-clean skipped ({e})', file=sys.stderr)

    user_cookies: list[str] = []
    post_ids: list[int] = []

    for i in range(NUM_USERS):
        username = f'bench_{i}'
        email    = f'{username}@bench.local'

        code, _, _ = request_json('POST', f'{base}/auth/register',
            body={'username': username, 'email': email, 'password': PASSWORD})
        if code not in (201,):
            raise SystemExit(f'register {username}: status {code}')

        cookies = login(base, username, PASSWORD)
        cookie_header = '; '.join(f'{k}={v}' for k, v in cookies.items())
        user_cookies.append(cookie_header)

        # Each user posts a slice of POSTS so the corpus has authors round-robined.
        slice_ = POSTS[i % len(POSTS):] + POSTS[:i % len(POSTS)]
        csrf = cookies.get('csrf_token', '')
        for title, content in slice_:
            code, body, _ = request_json('POST', f'{base}/posts',
                body={'title': title, 'content': content},
                cookies=cookies,
                extra_headers={'X-CSRF-Token': csrf})
            if code != 201:
                raise SystemExit(f'create post by {username}: status {code} body={body!r}')
            post_id = json.loads(body)['post']['id']
            post_ids.append(post_id)

    seed = {
        'post_ids':         post_ids,
        'search_terms':     ['postgresql', 'tsvector', 'argon2id',
                             'websockets', 'vue', 'gin'],
        'session_cookies':  user_cookies,
    }
    Path(args.out).write_text(json.dumps(seed, indent=2))
    print(f'seeded {len(user_cookies)} users, {len(post_ids)} posts → {args.out}')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
