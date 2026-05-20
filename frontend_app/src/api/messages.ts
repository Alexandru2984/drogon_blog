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
  other_user?: { id: number; username: string; profile_image?: string }
}

export interface InboxItem {
  id: number
  content: string
  is_read: number
  created_at: string
  sender?: { id: number; username: string; profile_image?: string }
  receiver?: { id: number; username: string; profile_image?: string }
}

export const messagesApi = {
  received() {
    return api.get<{ messages: InboxItem[] }>('/messages/received')
      .then(r => r.data.messages)
  },
  sent() {
    return api.get<{ messages: InboxItem[] }>('/messages/sent')
      .then(r => r.data.messages)
  },
  conversation(userId: number) {
    return api.get<ConversationPayload>(`/messages/conversation/${userId}`)
      .then(r => r.data)
  },
  send(receiverId: number, content: string) {
    return api.post<{ message: string; msg: MessageRow }>(
      '/messages',
      { receiver_id: receiverId, content }
    ).then(r => r.data.msg)
  },
  markRead(id: number) {
    return api.put(`/messages/${id}/read`).then(r => r.data)
  },
}
