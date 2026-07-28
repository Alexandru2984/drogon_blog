/* Service worker: app shell offline, nothing else.
 *
 * Scope is deliberately narrow. The temptation with a service worker is to
 * cache API responses too, but this application serves per-user, per-session
 * data behind a session cookie — caching /auth/me or /messages in a shared
 * Cache Storage bucket risks showing one account's data to the next person
 * to use the browser. So: static build output only, and every request that
 * is not a same-origin static asset goes straight to the network.
 *
 * The cache name carries a build stamp. Vite hashes asset filenames, so a
 * deploy produces new URLs and old entries would otherwise accumulate for
 * ever; bumping the name on activate clears them in one step.
 */

const VERSION = 'v1';
const CACHE = `blog-shell-${VERSION}`;
const OFFLINE_URL = '/offline.html';

const PRECACHE = ['/', OFFLINE_URL, '/manifest.webmanifest', '/icons/icon-192.png'];

self.addEventListener('install', (event) => {
  event.waitUntil(
    caches
      .open(CACHE)
      // addAll rejects the whole batch if any single entry 404s, which
      // would leave the worker permanently failing to install. Adding them
      // individually means a missing optional file costs that file only.
      .then((cache) => Promise.allSettled(PRECACHE.map((u) => cache.add(u))))
      .then(() => self.skipWaiting()),
  );
});

self.addEventListener('activate', (event) => {
  event.waitUntil(
    caches
      .keys()
      .then((keys) =>
        Promise.all(keys.filter((k) => k !== CACHE).map((k) => caches.delete(k))),
      )
      .then(() => self.clients.claim()),
  );
});

function isStaticAsset(url) {
  return (
    url.pathname.startsWith('/assets/') ||
    url.pathname.startsWith('/icons/') ||
    url.pathname === '/manifest.webmanifest'
  );
}

self.addEventListener('fetch', (event) => {
  const req = event.request;
  if (req.method !== 'GET') return;

  const url = new URL(req.url);
  if (url.origin !== self.location.origin) return;

  // Never cache uploads: they are user content served from the same origin
  // and can be moderated away, at which point a cached copy would keep
  // serving something that has been taken down.
  if (url.pathname.startsWith('/uploads/')) return;

  // Navigations: network first, so a fresh deploy is picked up immediately,
  // falling back to the offline page only when the network is actually
  // unavailable.
  if (req.mode === 'navigate') {
    event.respondWith(
      fetch(req).catch(() =>
        caches.match(OFFLINE_URL).then((r) => r || caches.match('/')),
      ),
    );
    return;
  }

  // Hashed build assets: cache first. The filename changes when the content
  // does, so a hit is never stale.
  if (isStaticAsset(url)) {
    event.respondWith(
      caches.match(req).then(
        (hit) =>
          hit ||
          fetch(req).then((resp) => {
            if (resp.ok && resp.type === 'basic') {
              const copy = resp.clone();
              caches.open(CACHE).then((c) => c.put(req, copy));
            }
            return resp;
          }),
      ),
    );
  }

  // Everything else — the API — is left to the network untouched.
});
