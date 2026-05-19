import axios from 'axios'

export const api = axios.create({
  // Same-origin in production; Vite proxies in dev. baseURL '' lets requests
  // be relative to the current host, so cookies (JSESSIONID) flow naturally.
  baseURL: '',
  withCredentials: true,
  headers: { 'Content-Type': 'application/json' },
})

export type ApiError = { error?: string; message?: string }
