import { api } from './client'

export type ReportTargetType = 'post' | 'comment' | 'user'
export type ReportReason = 'spam' | 'harassment' | 'illegal' | 'sexual' | 'other'
export type ReportStatus = 'open' | 'actioned' | 'dismissed'

export interface Report {
  id: number
  target_type: ReportTargetType
  target_id: number
  reason: ReportReason
  detail: string
  status: ReportStatus
  created_at: string
  reporter: string
  // For a comment report, the post the comment is on. Absent for other
  // targets, or when the comment has since been deleted outright.
  post_id?: number
}

export const moderationApi = {
  // Any authenticated user.
  createReport(payload: { target_type: ReportTargetType; target_id: number; reason: ReportReason; detail?: string }) {
    return api.post('/reports', payload).then(r => r.data)
  },

  // Everything below is moderator-and-above; the backend answers 404 (not
  // 403) to anyone without the role, same as probing a nonexistent route.
  listReports(status: ReportStatus = 'open') {
    return api.get<{ reports: Report[] }>('/admin/reports', { params: { status } }).then(r => r.data.reports)
  },
  resolveReport(id: number, status: 'actioned' | 'dismissed', note?: string) {
    return api.post(`/admin/reports/${id}/resolve`, { status, note }).then(r => r.data)
  },
  hidePost(id: number, reason?: string) {
    return api.post(`/admin/posts/${id}/hide`, { reason }).then(r => r.data)
  },
  unhidePost(id: number) {
    return api.post(`/admin/posts/${id}/unhide`).then(r => r.data)
  },
  hideComment(id: number, reason?: string) {
    return api.post(`/admin/comments/${id}/hide`, { reason }).then(r => r.data)
  },
  unhideComment(id: number) {
    return api.post(`/admin/comments/${id}/unhide`).then(r => r.data)
  },
  banUser(id: number, days: number, reason?: string) {
    return api.post<{ message: string; days: number; revoked_sessions: number }>(
      `/admin/users/${id}/ban`, { days, reason }).then(r => r.data)
  },
  unbanUser(id: number) {
    return api.post(`/admin/users/${id}/unban`).then(r => r.data)
  },
}
