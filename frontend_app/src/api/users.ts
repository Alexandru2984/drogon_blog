import { api } from './client'
import type { User } from './auth'

export const usersApi = {
  get(id: number) {
    return api.get<User>(`/users/${id}`).then(r => r.data)
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
