import { api } from './client'

export interface SessionEntry {
  sid: string
  created_at: string
  last_seen_at: string
  ip: string
  user_agent: string
  /** True for the session making the request — the UI must not offer to
   *  revoke it, since doing so would sign the user out of the page they
   *  are standing on. "Sign out everywhere else" covers that case. */
  current: boolean
}

export const accountApi = {
  async changePassword(currentPassword: string, newPassword: string) {
    const { data } = await api.post<{ message: string; revoked_sessions: number }>(
      '/auth/change-password',
      { current_password: currentPassword, new_password: newPassword },
    )
    return data
  },

  async listSessions() {
    const { data } = await api.get<{ sessions: SessionEntry[] }>('/auth/sessions')
    return data.sessions
  },

  async revokeSession(sid: string) {
    const { data } = await api.post<{ message: string }>(
      '/auth/sessions/revoke', { sid },
    )
    return data
  },

  async revokeOtherSessions() {
    const { data } = await api.post<{ message: string; revoked_sessions: number }>(
      '/auth/sessions/revoke-others', {},
    )
    return data
  },
}
