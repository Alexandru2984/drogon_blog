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

  // Everything the account holds, as one JSON document. POST, not GET,
  // because it carries the password: a password in a URL ends up in the
  // browser history, the Referer header and every proxy log on the way.
  //
  // Returned as a Blob so the file reaches the disk byte-for-byte as the
  // server wrote it. Parsing it into an object here and re-serialising to
  // download it would hand the user a file that is subtly not the one the
  // server produced.
  async exportData(password: string) {
    try {
      const res = await api.post('/account/export', { password }, {
        responseType: 'blob',
      })
      const disposition = String(res.headers['content-disposition'] ?? '')
      const match = /filename="([^"]+)"/.exec(disposition)
      return {
        blob: res.data as Blob,
        filename: match?.[1] ?? 'blog-export.json',
      }
    } catch (e: any) {
      // responseType 'blob' applies to error bodies too, so a 403 arrives
      // as a Blob and `e.response.data.error` is undefined — the caller
      // would show "something went wrong" for a wrong password. Unpack it
      // here so every caller does not have to know that.
      const body = e?.response?.data
      if (body instanceof Blob) {
        try {
          const parsed = JSON.parse(await body.text())
          if (parsed?.error) e.response.data = parsed
        } catch { /* not JSON: leave the original error alone */ }
      }
      throw e
    }
  },

  // Irreversible. `confirm` must be the username, typed out — the password
  // is the security control, this is the one that stops a mis-click.
  async deleteAccount(password: string, confirm: string) {
    const { data } = await api.post<{
      message: string
      deleted: Record<string, number>
    }>('/account/delete', { password, confirm })
    return data
  },
}
