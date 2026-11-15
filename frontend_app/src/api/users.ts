import { api } from './client'
import type { User } from './auth'

export interface UserSummary {
  id: number
  username: string
  bio?: string
  profile_image?: string
  online?: boolean
}

export const usersApi = {
  get(id: number) {
    return api.get<User & { online?: boolean }>(`/users/${id}`).then(r => r.data)
  },
  search(q?: string, before?: number, limit = 20) {
    return api.get<{ users: UserSummary[]; next_cursor?: number | null }>(
      '/users',
      { params: { ...(q ? { q } : {}), ...(before ? { before } : {}), limit } },
    ).then(r => r.data)
  },
  updateProfile(payload: { email?: string; bio?: string; current_password?: string }) {
    return api.put('/users/profile', payload).then(r => r.data)
  },
  uploadImage(file: File) {
    const form = new FormData()
    form.append('file', file)
    return api.post('/users/profile/image', form, {
      headers: { 'Content-Type': 'multipart/form-data' },
    }).then(r => r.data)
  },
}
