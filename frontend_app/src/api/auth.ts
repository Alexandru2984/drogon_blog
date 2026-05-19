import { api } from './client'

export interface User {
  id: number
  username: string
  email: string
  bio?: string
  profile_image?: string
}

export const authApi = {
  register(payload: { username: string; email: string; password: string }) {
    return api.post('/auth/register', payload).then(r => r.data)
  },
  login(payload: { username: string; password: string }) {
    return api.post<{ message: string; user: User }>('/auth/login', payload).then(r => r.data)
  },
  logout() {
    return api.post('/auth/logout').then(r => r.data)
  },
  me() {
    return api.get<User>('/auth/me').then(r => r.data)
  },
  verifyEmail(token: string) {
    return api.post('/auth/verify-email', { token }).then(r => r.data)
  },
  requestReset(email: string) {
    return api.post('/auth/request-reset', { email }).then(r => r.data)
  },
  resetPassword(token: string, password: string) {
    return api.post('/auth/reset-password', { token, password }).then(r => r.data)
  },
}
