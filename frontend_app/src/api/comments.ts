import { api } from './client'

export interface Comment {
  id: number
  post_id: number
  user_id: number
  content: string
  created_at: string
  author?: { id: number; username: string; profile_image?: string }
}

export const commentsApi = {
  forPost(postId: number) {
    return api.get<{ comments: Comment[] }>(`/posts/${postId}/comments`).then(r => r.data.comments)
  },
  create(postId: number, content: string) {
    return api.post(`/posts/${postId}/comments`, { content }).then(r => r.data)
  },
  remove(commentId: number) {
    return api.delete(`/comments/${commentId}`).then(r => r.data)
  },
}
