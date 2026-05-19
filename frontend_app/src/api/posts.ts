import { api } from './client'

export interface PostAuthor {
  id: number
  username: string
  profile_image?: string
}

export interface Post {
  id: number
  title: string
  content: string
  created_at: string
  updated_at: string
  author?: PostAuthor
}

export const postsApi = {
  list() {
    return api.get<{ posts: Post[] }>('/posts').then(r => r.data.posts)
  },
  byUser(userId: number) {
    return api.get<{ posts: Post[] }>(`/posts/user/${userId}`).then(r => r.data.posts)
  },
  get(id: number) {
    return api.get<Post>(`/posts/${id}`).then(r => r.data)
  },
  create(payload: { title: string; content: string }) {
    return api.post('/posts', payload).then(r => r.data)
  },
  update(id: number, payload: { title?: string; content?: string }) {
    return api.put(`/posts/${id}`, payload).then(r => r.data)
  },
  remove(id: number) {
    return api.delete(`/posts/${id}`).then(r => r.data)
  },
  like(id: number) {
    return api.post(`/posts/${id}/like`).then(r => r.data)
  },
  unlike(id: number) {
    return api.delete(`/posts/${id}/like`).then(r => r.data)
  },
  likesCount(id: number) {
    return api.get<{ post_id: number; likes_count: number }>(`/posts/${id}/likes`).then(r => r.data)
  },
}
