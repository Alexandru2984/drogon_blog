// k6 scenarios for the Drogon blog.
//
// Usage:
//   k6 run --vus 30 --duration 30s --tag scenario=feed_read     -e SCENARIO=feed_read     bench/scenarios.js
//   k6 run --vus 30 --duration 30s --tag scenario=post_view     -e SCENARIO=post_view     bench/scenarios.js
//   k6 run --vus 20 --duration 30s --tag scenario=search        -e SCENARIO=search        bench/scenarios.js
//   k6 run --vus 30 --duration 30s --tag scenario=auth_me_warm  -e SCENARIO=auth_me_warm  bench/scenarios.js
//
// run.sh wraps the whole flight including seeding.

import http  from 'k6/http'
import { check, sleep } from 'k6'
import { Rate } from 'k6/metrics'

const BASE     = __ENV.BASE_URL   || 'http://127.0.0.1:8092'
const SCENARIO = __ENV.SCENARIO   || 'feed_read'

// Seed data shape; populated by bench/seed.py. The IDs file is loaded at init
// time so each VU starts with the same fixed corpus.
const seed = JSON.parse(open('./seed.json'))

const errors = new Rate('blog_errors')

function pick(arr) { return arr[Math.floor(Math.random() * arr.length)] }

// Per-scenario strict thresholds, used when K6_STRICT=1. Each k6 run
// drives a single SCENARIO, so the metric names stay bare (no tag
// filter) — they apply to the whole run.
//
// Numbers are chosen for the GitHub Actions ubuntu-24.04 runner
// profile (4 vCPU, shared host), not for the prod VPS — the goal is
// to catch order-of-magnitude regressions, not absolute parity with
// BENCHMARKS.md. Bump these if the runner class changes or the suite
// gets noisy; loosen only with a paired note in bench/README.md
// explaining why.
const STRICT_THRESHOLDS = {
  feed_read: {
    'http_req_duration': ['p(95)<150'],
    'http_reqs':         ['rate>500'], // RPS floor
  },
  post_view: {
    'http_req_duration': ['p(95)<150'],
    'http_reqs':         ['rate>500'],
  },
  search: {
    'http_req_duration': ['p(95)<300'],
    'http_reqs':         ['rate>200'],
  },
  feed_read_warm: {
    'http_req_duration': ['p(95)<100'],
    'http_reqs':         ['rate>800'],
  },
}

function buildThresholds() {
  // Always-on guards. blog_errors is the synthetic rate we increment
  // on a failed check; http_req_failed is k6's built-in transport
  // failure rate. Both should sit close to zero.
  const base = {
    'blog_errors':       ['rate<0.005'],
    'http_req_failed':   ['rate<0.005'],
    'http_req_duration': ['p(95)<500'],
  }
  if (__ENV.K6_STRICT === '1' && STRICT_THRESHOLDS[SCENARIO]) {
    // Strict overrides bake replace the lax defaults for the metrics
    // they touch (http_req_duration in particular). Object.assign
    // semantics: later keys win.
    return Object.assign(base, STRICT_THRESHOLDS[SCENARIO])
  }
  return base
}

export const options = {
  // Sensible defaults — the cli flags override these per scenario.
  summaryTrendStats: ['avg', 'p(50)', 'p(95)', 'p(99)', 'max'],
  thresholds: buildThresholds(),
}

function getFeed() {
  const r = http.get(`${BASE}/posts`, { tags: { name: 'GET /posts' } })
  const ok = check(r, { 'feed 200': res => res.status === 200 })
  errors.add(!ok)
}

function getPost() {
  const id = pick(seed.post_ids)
  const r = http.get(`${BASE}/posts/${id}`, { tags: { name: 'GET /posts/{id}' } })
  const ok = check(r, { 'post 200': res => res.status === 200 })
  errors.add(!ok)
}

function search() {
  const q = pick(seed.search_terms)
  const r = http.get(`${BASE}/posts/search?q=${encodeURIComponent(q)}`,
    { tags: { name: 'GET /posts/search' } })
  const ok = check(r, { 'search 200': res => res.status === 200 })
  errors.add(!ok)
}

// "Warm cache" variants: each VU caches the ETag it sees on its first
// hit and replays it via If-None-Match. The expected hot-path status
// becomes 304, so the assertion flips. Models a returning client / CDN
// edge that holds a valid ETag.
//
// __VU is a 1-based unique VU index per k6; we keep one ETag per VU
// per resource so VUs don't fight over a shared variable (and so
// resources that don't collide — different post ids, different search
// terms — keep their own tag).
const cache = {} // { 'kind|key': etag }
function etagGet(kind, key, url, name) {
  const ck = `${kind}|${key}`
  let headers = {}
  if (cache[ck]) headers['If-None-Match'] = cache[ck]
  const r = http.get(url, { headers, tags: { name } })
  // Update only when the server gives us a non-empty ETag (200 or 304
  // both echo it back). A 200 with a new tag means the resource moved
  // between two of our requests — fine, we just refresh and continue.
  const fresh = r.headers['Etag'] || r.headers['ETag']
  if (fresh) cache[ck] = fresh
  return r
}

function getFeedWarm() {
  const r = etagGet('feed', 'list',
    `${BASE}/posts`, 'GET /posts (warm)')
  // First iter is 200 (cache miss), subsequent are 304 unless the
  // resource was mutated.
  const ok = check(r, { 'feed 200/304': res => res.status === 200 || res.status === 304 })
  errors.add(!ok)
}

function getPostWarm() {
  const id = pick(seed.post_ids)
  const r = etagGet('post', String(id),
    `${BASE}/posts/${id}`, 'GET /posts/{id} (warm)')
  const ok = check(r, { 'post 200/304': res => res.status === 200 || res.status === 304 })
  errors.add(!ok)
}

function searchWarm() {
  const q = pick(seed.search_terms)
  const r = etagGet('search', q,
    `${BASE}/posts/search?q=${encodeURIComponent(q)}`, 'GET /posts/search (warm)')
  const ok = check(r, { 'search 200/304': res => res.status === 200 || res.status === 304 })
  errors.add(!ok)
}

function authMeWarm() {
  const cookie = pick(seed.session_cookies)
  const r = http.get(`${BASE}/auth/me`, {
    tags: { name: 'GET /auth/me' },
    headers: { Cookie: cookie },
  })
  const ok = check(r, { 'me 200': res => res.status === 200 })
  errors.add(!ok)
}

const scenarios = {
  feed_read:        getFeed,
  post_view:        getPost,
  search:           search,
  auth_me_warm:     authMeWarm,
  feed_read_warm:   getFeedWarm,
  post_view_warm:   getPostWarm,
  search_warm:      searchWarm,
}

export default function () {
  const fn = scenarios[SCENARIO]
  if (!fn) throw new Error(`Unknown scenario: ${SCENARIO}`)
  fn()
  sleep(0)        // tight loop; rely on --vus + --duration to shape load
}
