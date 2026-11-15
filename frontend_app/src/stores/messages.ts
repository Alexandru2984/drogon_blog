import { defineStore } from 'pinia'
import { computed, ref } from 'vue'
import { messagesApi, type MessageRow } from '@/api/messages'
import { usersApi } from '@/api/users'
import type { Comment } from '@/api/comments'
import { useAuthStore } from './auth'

export interface ConversationPeer {
  id: number
  username?: string
  profile_image?: string
  online?: boolean
}

export interface Conversation {
  peer:        ConversationPeer
  messages:    MessageRow[]
  unread:      number
  nextCursor?: number | null
}

// Singleton state for the lifetime of the SPA. Connections are owned by the
// store; views just react to refs.
let socket: WebSocket | null = null
let reconnectMs = 1000
let keepalive: ReturnType<typeof setInterval> | null = null
let manualClose = false
let onlineListenerBound = false

const desiredPostSubs = new Set<number>()

const MAX_CONSECUTIVE_FAILURES = 6
let consecutiveFailures = 0

let onNotification: (() => void) | null = null

export const useMessagesStore = defineStore('messages', () => {
  const conversations = ref<Map<number, Conversation>>(new Map())
  const connected     = ref(false)
  const liveUnavailable = ref(false)
  const liveCommentsByPost = ref<Map<number, Comment[]>>(new Map())
  const auth = useAuthStore()

  const totalUnread = computed(() => {
    let n = 0
    for (const c of conversations.value.values()) n += c.unread
    return n
  })

  function peerIdFor(m: MessageRow): number | null {
    if (!auth.user) return null
    return m.sender_id === auth.user.id ? m.receiver_id : m.sender_id
  }

  function upsertPeer(peer: ConversationPeer) {
    const existing = conversations.value.get(peer.id)
    if (existing) {
      existing.peer = { ...existing.peer, ...peer }
      conversations.value.set(peer.id, { ...existing })
    } else {
      conversations.value.set(peer.id, { peer, messages: [], unread: 0, nextCursor: null })
    }
  }

  function ingest(m: MessageRow, opts: { markRead?: boolean } = {}) {
    const peerId = peerIdFor(m)
    if (peerId == null) return
    const existing = conversations.value.get(peerId)
    const conv: Conversation = existing
      ? { ...existing, messages: [...existing.messages] }
      : { peer: { id: peerId }, messages: [], unread: 0, nextCursor: null }
    // Dedup on id (REST + WS can race for sender's own outbound message).
    const idx = conv.messages.findIndex(x => x.id === m.id)
    if (idx === -1) {
      conv.messages.push(m)
      conv.messages.sort((a, b) => a.id - b.id)
    } else {
      conv.messages[idx] = { ...conv.messages[idx], ...m }
    }
    const fromMe = auth.user && m.sender_id === auth.user.id
    if (!fromMe && !m.is_read && !opts.markRead && idx === -1) conv.unread += 1
    if (opts.markRead) conv.unread = 0
    conversations.value.set(peerId, conv)

    if (!conv.peer.username) {
      usersApi.get(peerId)
        .then(u => upsertPeer({
          id: u.id,
          username: u.username,
          profile_image: u.profile_image,
          online: u.online,
        }))
        .catch(() => { /* ignore — UI shows User #N */ })
    }
  }

  function clear() {
    conversations.value = new Map()
    liveCommentsByPost.value = new Map()
    desiredPostSubs.clear()
    liveUnavailable.value = false
    consecutiveFailures = 0
    reconnectMs = 1000
  }

  async function refreshInbox() {
    if (!auth.isAuthed) return
    try {
      const [recv, sent] = await Promise.all([
        messagesApi.received(),
        messagesApi.sent(),
      ])
      for (const m of recv) {
        if (m.sender) upsertPeer(m.sender)
        ingest({
          id: m.id,
          sender_id: m.sender!.id,
          receiver_id: auth.user!.id,
          content: m.content,
          is_read: m.is_read,
          created_at: m.created_at,
        })
      }
      for (const m of sent) {
        if (m.receiver) upsertPeer(m.receiver)
        ingest({
          id: m.id,
          sender_id: auth.user!.id,
          receiver_id: m.receiver!.id,
          content: m.content,
          is_read: m.is_read,
          created_at: m.created_at,
        })
      }
    } catch { /* ignore — view will retry */ }
  }

  async function openConversation(peerId: number) {
    if (!auth.isAuthed) return
    const data = await messagesApi.conversation(peerId)
    if (data.other_user) upsertPeer(data.other_user)
    for (const m of data.messages) ingest(m, { markRead: false })
    const conv = conversations.value.get(peerId)
    if (conv) {
      conv.nextCursor = data.next_cursor ?? null
      const unreadIds = conv.messages
        .filter(m => m.receiver_id === auth.user!.id && !m.is_read)
        .map(m => m.id)
      for (const m of conv.messages) {
        if (m.receiver_id === auth.user!.id) m.is_read = 1
      }
      conv.unread = 0
      conversations.value.set(peerId, { ...conv })
      for (const id of unreadIds) {
        messagesApi.markRead(id).catch(() => {})
      }
    }
  }

  async function loadOlderMessages(peerId: number): Promise<boolean> {
    if (!auth.isAuthed) return false
    const conv = conversations.value.get(peerId)
    if (!conv || !conv.nextCursor) return false
    const data = await messagesApi.conversation(peerId, conv.nextCursor)
    if (data.other_user) upsertPeer(data.other_user)
    for (const m of data.messages) ingest(m, { markRead: false })
    const updated = conversations.value.get(peerId)
    if (updated) {
      updated.nextCursor = data.next_cursor ?? null
      conversations.value.set(peerId, { ...updated })
    }
    return Boolean(data.next_cursor)
  }

  async function send(peerId: number, content: string) {
    const trimmed = content.trim()
    if (!trimmed) return
    const msg = await messagesApi.send(peerId, trimmed)
    ingest(msg)
  }

  async function removeMessage(peerId: number, messageId: number) {
    await messagesApi.remove(messageId)
    const conv = conversations.value.get(peerId)
    if (!conv) return
    conv.messages = conv.messages.filter(m => m.id !== messageId)
    conversations.value.set(peerId, { ...conv })
  }

  function retryLive() {
    liveUnavailable.value = false
    consecutiveFailures = 0
    reconnectMs = 1000
    connectSocket()
  }

  function connectSocket() {
    if (!auth.isAuthed) return
    if (liveUnavailable.value) return
    if (socket && (socket.readyState === WebSocket.OPEN ||
                   socket.readyState === WebSocket.CONNECTING)) return

    if (!onlineListenerBound && typeof window !== 'undefined') {
      onlineListenerBound = true
      window.addEventListener('online', () => {
        if (auth.isAuthed && (!connected.value || liveUnavailable.value)) {
          retryLive()
        }
      })
    }

    manualClose = false
    const proto = window.location.protocol === 'https:' ? 'wss:' : 'ws:'
    socket = new WebSocket(`${proto}//${window.location.host}/ws/messages`)

    socket.onopen = () => {
      connected.value = true
      reconnectMs = 1000
      consecutiveFailures = 0
      for (const postId of desiredPostSubs) {
        socket!.send(JSON.stringify({ type: 'subscribe_post', post_id: postId }))
      }
      if (keepalive) clearInterval(keepalive)
      keepalive = setInterval(() => {
        if (socket && socket.readyState === WebSocket.OPEN) socket.send('ping')
      }, 25_000)
    }

    socket.onmessage = (ev) => {
      const data = typeof ev.data === 'string' ? ev.data : ''
      if (data === 'pong') return
      let env: any
      try { env = JSON.parse(data) } catch { return }
      if (env?.type === 'message' && env.message) {
        ingest(env.message as MessageRow)
      } else if (env?.type === 'notification') {
        onNotification?.()
      } else if (env?.type === 'comment' && env.comment && typeof env.post_id === 'number') {
        const arr = liveCommentsByPost.value.get(env.post_id) ?? []
        arr.push(env.comment as Comment)
        liveCommentsByPost.value.set(env.post_id, arr)
      }
    }

    socket.onclose = (ev) => {
      const wasConnected = connected.value
      connected.value = false
      if (keepalive) { clearInterval(keepalive); keepalive = null }
      if (manualClose || !auth.isAuthed) return

      // 1008 Policy Violation means the session was revoked or unauthenticated.
      if (ev.code === 1008) {
        liveUnavailable.value = true
        return
      }

      if (wasConnected) consecutiveFailures = 0
      else if (++consecutiveFailures >= MAX_CONSECUTIVE_FAILURES) {
        liveUnavailable.value = true
        return
      }

      const delay = reconnectMs
      reconnectMs = Math.min(30_000, reconnectMs * 2)
      setTimeout(() => connectSocket(), delay)
    }

    socket.onerror = () => { /* surfaced via onclose */ }
  }

  function disconnectSocket() {
    manualClose = true
    if (keepalive) { clearInterval(keepalive); keepalive = null }
    if (socket) {
      try { socket.close() } catch { /* ignore */ }
      socket = null
    }
    connected.value = false
  }

  function subscribePost(postId: number) {
    desiredPostSubs.add(postId)
    if (!socket || socket.readyState !== WebSocket.OPEN) return
    socket.send(JSON.stringify({ type: 'subscribe_post', post_id: postId }))
  }

  function unsubscribePost(postId: number) {
    desiredPostSubs.delete(postId)
    liveCommentsByPost.value.delete(postId)
    if (!socket || socket.readyState !== WebSocket.OPEN) return
    socket.send(JSON.stringify({ type: 'unsubscribe_post', post_id: postId }))
  }

  function setNotificationHandler(fn: () => void) {
    onNotification = fn
  }

  return {
    conversations,
    connected,
    setNotificationHandler,
    liveUnavailable,
    totalUnread,
    liveCommentsByPost,
    upsertPeer,
    refreshInbox,
    openConversation,
    loadOlderMessages,
    send,
    removeMessage,
    retryLive,
    connectSocket,
    disconnectSocket,
    subscribePost,
    unsubscribePost,
    clear,
  }
})
