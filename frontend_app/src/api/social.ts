import { api } from './client'
import type { Post } from './posts'

export type NotificationKind = 'comment' | 'reply' | 'follow' | 'new_post' | 'like'

export interface NotificationActor {
  id: number
  username: string
  profile_image?: string
}

export interface AppNotification {
  id: number
  kind: NotificationKind
  created_at: string
  read: boolean
  actor?: NotificationActor
  post_id?: number
  comment_id?: number
  // Resolved server-side so a list of fifty rows is one request, not a
  // hundred.
  post_title?: string
  comment_preview?: string
}

export interface FollowStats {
  followers: number
  following: number
  is_following: boolean
}

export const socialApi = {
  // ---- bookmarks ----
  bookmarks() {
    return api.get<{ posts: Post[] }>('/bookmarks').then(r => r.data.posts)
  },
  // Both directions are idempotent server-side, so the caller never has to
  // check the current state before acting.
  addBookmark(postId: number) {
    return api.post<{ bookmarked: boolean }>(`/posts/${postId}/bookmark`).then(r => r.data)
  },
  removeBookmark(postId: number) {
    return api.delete<{ bookmarked: boolean }>(`/posts/${postId}/bookmark`).then(r => r.data)
  },

  // ---- follows ----
  follow(userId: number) {
    return api.post<{ following: boolean }>(`/users/${userId}/follow`).then(r => r.data)
  },
  unfollow(userId: number) {
    return api.delete<{ following: boolean }>(`/users/${userId}/follow`).then(r => r.data)
  },
  followStats(userId: number) {
    return api.get<FollowStats>(`/users/${userId}/follow-stats`).then(r => r.data)
  },
  followingFeed() {
    return api.get<{ posts: Post[] }>('/feed/following').then(r => r.data.posts)
  },

  // ---- notifications ----
  notifications(before?: number) {
    const params: Record<string, string> = {}
    if (before) params.before = String(before)
    return api
      .get<{ notifications: AppNotification[]; unread: number }>('/notifications', { params })
      .then(r => r.data)
  },
  unreadCount() {
    return api.get<{ unread: number }>('/notifications/unread').then(r => r.data.unread)
  },
  markRead(id: number) {
    return api.post<{ unread: number }>(`/notifications/${id}/read`).then(r => r.data.unread)
  },
  markAllRead() {
    return api.post<{ marked: number; unread: number }>('/notifications/read-all').then(r => r.data)
  },
}
