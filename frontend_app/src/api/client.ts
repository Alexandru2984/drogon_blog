import axios from 'axios'

export const api = axios.create({
  // Production stays same-origin. Development uses one reserved prefix that
  // Vite strips before proxying to Drogon. A single catch-all prevents every
  // new top-level API route from needing another proxy entry (the missing
  // /tags, /flags, /bookmarks, /feed, /notifications and /account entries
  // previously returned Vite's index.html to Axios).
  baseURL: import.meta.env.DEV ? '/__api' : '',
  withCredentials: true,
  headers: { 'Content-Type': 'application/json' },
})

// Double-submit CSRF: read the readable cookie set by the backend on login /
// session bootstrap and echo it in the X-CSRF-Token header on mutating verbs.
function readCookie(name: string): string | null {
  const escaped = name.replace(/[.*+?^${}()|[\]\\]/g, '\\$&')
  const m = document.cookie.match(new RegExp('(?:^|;\\s*)' + escaped + '=([^;]+)'))
  return m ? decodeURIComponent(m[1]) : null
}

// The backend names the CSRF cookie `__Host-csrf_token` when it is serving
// over TLS and plain `csrf_token` otherwise (a `__Host-` cookie without the
// Secure attribute is rejected by the browser, and dev / CI run on plain
// HTTP). Prefer the hardened name and fall back, so one build works against
// both. See helpers/Security.h::csrfCookieName().
function readCsrfToken(): string | null {
  return readCookie('__Host-csrf_token') ?? readCookie('csrf_token')
}

api.interceptors.request.use((config) => {
  const method = (config.method || 'get').toLowerCase()
  if (method !== 'get' && method !== 'head' && method !== 'options') {
    const token = readCsrfToken()
    if (token) {
      config.headers = config.headers ?? {}
      ;(config.headers as Record<string, string>)['X-CSRF-Token'] = token
    }
  }
  return config
})

export type ApiError = { error?: string; message?: string }
