import { api } from './client'

export interface Comment {
  id: number
  post_id: number
  user_id: number
  content: string
  created_at: string
  // Null for a top-level comment. The client nests by this; the server sends
  // a flat list so the collection's ETag stays a function of the contents
  // rather than of the tree shape.
  parent_id?: number | null
  author?: { id: number; username: string; profile_image?: string }
  // A deleted comment that had replies, kept so they still have a parent.
  // It carries no author and can be neither edited nor replied to.
  deleted?: boolean
}

export const commentsApi = {
  forPost(postId: number) {
    return api.get<{ comments: Comment[] }>(`/posts/${postId}/comments`).then(r => r.data.comments)
  },
  // parentId makes it a reply. The server checks that the parent is a
  // visible comment on this same post.
  create(postId: number, content: string, parentId?: number) {
    const body: { content: string; parent_id?: number } = { content }
    if (parentId) body.parent_id = parentId
    return api.post(`/posts/${postId}/comments`, body).then(r => r.data)
  },
  // The server re-checks ownership and refuses an empty body or a deleted
  // comment.
  update(commentId: number, content: string) {
    return api.put<{ comment: { id: number; content: string } }>(`/comments/${commentId}`, { content })
      .then(r => r.data.comment)
  },
  // `tombstoned` is true when the comment had replies: it stays in the thread
  // as "[deleted]" so they keep their parent.
  remove(commentId: number) {
    return api.delete<{ message: string; tombstoned: boolean }>(`/comments/${commentId}`).then(r => r.data)
  },
}
