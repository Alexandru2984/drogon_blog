import { api } from './client'

export interface MessageRow {
  id: number
  sender_id: number
  receiver_id: number
  content: string
  is_read: number
  created_at: string
}

export interface ConversationPayload {
  messages: MessageRow[]
  next_cursor?: number | null
  other_user?: { id: number; username: string; profile_image?: string; online?: boolean }
}

export interface InboxItem {
  id: number
  content: string
  is_read: number
  created_at: string
  sender?: { id: number; username: string; profile_image?: string; online?: boolean }
  receiver?: { id: number; username: string; profile_image?: string; online?: boolean }
}

export const messagesApi = {
  received(before?: number, limit = 50) {
    return api.get<{ messages: InboxItem[]; next_cursor?: number | null }>(
      '/messages/received',
      { params: { ...(before ? { before } : {}), limit } },
    ).then(r => r.data.messages)
  },
  sent(before?: number, limit = 50) {
    return api.get<{ messages: InboxItem[]; next_cursor?: number | null }>(
      '/messages/sent',
      { params: { ...(before ? { before } : {}), limit } },
    ).then(r => r.data.messages)
  },
  conversation(userId: number, before?: number, limit = 50) {
    return api.get<ConversationPayload>(
      `/messages/conversation/${userId}`,
      { params: { ...(before ? { before } : {}), limit } },
    ).then(r => r.data)
  },
  send(receiverId: number, content: string) {
    return api.post<{ message: string; msg: MessageRow }>(
      '/messages',
      { receiver_id: receiverId, content },
    ).then(r => r.data.msg)
  },
  markRead(id: number) {
    return api.put(`/messages/${id}/read`).then(r => r.data)
  },
  remove(id: number) {
    return api.delete<{ message: string }>(`/messages/${id}`).then(r => r.data)
  },
}
