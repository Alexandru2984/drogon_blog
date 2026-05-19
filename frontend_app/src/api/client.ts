import axios from 'axios'

export const api = axios.create({
  // Same-origin in production; Vite proxies in dev. baseURL '' lets requests
  // be relative to the current host, so cookies (JSESSIONID) flow naturally.
  baseURL: '',
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

api.interceptors.request.use((config) => {
  const method = (config.method || 'get').toLowerCase()
  if (method !== 'get' && method !== 'head' && method !== 'options') {
    const token = readCookie('csrf_token')
    if (token) {
      config.headers = config.headers ?? {}
      ;(config.headers as Record<string, string>)['X-CSRF-Token'] = token
    }
  }
  return config
})

export type ApiError = { error?: string; message?: string }
