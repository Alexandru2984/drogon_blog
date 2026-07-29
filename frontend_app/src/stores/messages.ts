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
}

export interface Conversation {
  peer:     ConversationPeer
  messages: MessageRow[]
  unread:   number
}

// Singleton state for the lifetime of the SPA. Connections are owned by the
// store; views just react to refs.
let socket: WebSocket | null = null
let reconnectMs = 1000
let keepalive: ReturnType<typeof setInterval> | null = null
let manualClose = false

// Posts whose live comments this client wants. The socket may not be open at
// the moment a view asks — PostView subscribes in onMounted, which for a cold
// page load runs long before the handshake completes — so the intent is kept
// here and replayed on open. Previously the request was simply dropped and
// live comments never arrived on a first page view.
const desiredPostSubs = new Set<number>()

// How many consecutive failed handshakes before we stop trying. When the
// endpoint is unreachable for a structural reason (a proxy that does not
// forward the upgrade, WebSockets disabled at the CDN) retrying forever
// means every visitor burns a failed request every 30 s for the whole
// session and fills the console with errors, which buries real ones.
const MAX_CONSECUTIVE_FAILURES = 6
let consecutiveFailures = 0

export const useMessagesStore = defineStore('messages', () => {
  const conversations = ref<Map<number, Conversation>>(new Map())
  const connected     = ref(false)
  // Set once we have given up reconnecting. Lets the UI say "no live
  // updates" instead of a permanently "connecting…" indicator that never
  // resolves. Everything else on the page still works over REST.
  const liveUnavailable = ref(false)
  // Live comments pushed from the server, keyed by post id. PostView watches
  // its own slice and appends entries that aren't already on screen. The
  // store doesn't try to be the source of truth for the full comment list —
  // it's a one-way delivery surface.
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
      conversations.value.set(peer.id, existing)
    } else {
      conversations.value.set(peer.id, { peer, messages: [], unread: 0 })
    }
  }

  function ingest(m: MessageRow, opts: { markRead?: boolean } = {}) {
    const peerId = peerIdFor(m)
    if (peerId == null) return
    const existing = conversations.value.get(peerId)
    const conv = existing
                ?? { peer: { id: peerId }, messages: [], unread: 0 }
    // Dedup on id (REST + WS can race for sender's own outbound message).
    if (!conv.messages.some(x => x.id === m.id)) {
      conv.messages.push(m)
      conv.messages.sort((a, b) => a.id - b.id)
    }
    const fromMe = auth.user && m.sender_id === auth.user.id
    if (!fromMe && !m.is_read && !opts.markRead) conv.unread += 1
    if (opts.markRead) conv.unread = 0
    conversations.value.set(peerId, conv)

    // WS push payloads carry only the raw `messages` row — no
    // peer username / avatar (the trg_messages_notify trigger
    // truncates the JSON to fit under PG's 8 KiB NOTIFY cap, so
    // shipping the JOIN'd author is impractical there). Enrich
    // lazily on the client: when we see a new peer with no
    // username yet, fire-and-forget GET /users/{id}. Failure is
    // non-fatal — the UI degrades to "User #N" which is the same
    // shape it shows when the lookup is in flight.
    if (!conv.peer.username) {
      usersApi.get(peerId)
        .then(u => upsertPeer({ id: u.id, username: u.username, profile_image: u.profile_image }))
        .catch(() => { /* ignore — UI shows User #N */ })
    }
  }

  function clear() {
    conversations.value = new Map()
    liveCommentsByPost.value = new Map()
    desiredPostSubs.clear()
    // A new sign-in deserves a fresh attempt: the previous session may have
    // given up while the network was down.
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
    // Optimistic local mark-as-read for messages addressed to us
    const conv = conversations.value.get(peerId)
    if (conv) {
      const unreadIds = conv.messages
        .filter(m => m.receiver_id === auth.user!.id && !m.is_read)
        .map(m => m.id)
      conv.unread = 0
      conversations.value.set(peerId, conv)
      for (const id of unreadIds) {
        // Best-effort server-side mark-as-read; failures are non-fatal.
        messagesApi.markRead(id).catch(() => {})
      }
    }
  }

  async function send(peerId: number, content: string) {
    const trimmed = content.trim()
    if (!trimmed) return
    const msg = await messagesApi.send(peerId, trimmed)
    ingest(msg)
  }

  function connectSocket() {
    if (!auth.isAuthed) return
    if (liveUnavailable.value) return
    if (socket && (socket.readyState === WebSocket.OPEN ||
                   socket.readyState === WebSocket.CONNECTING)) return

    manualClose = false
    const proto = window.location.protocol === 'https:' ? 'wss:' : 'ws:'
    socket = new WebSocket(`${proto}//${window.location.host}/ws/messages`)

    socket.onopen = () => {
      connected.value = true
      reconnectMs = 1000
      consecutiveFailures = 0
      // Replay whatever views asked for while the socket was down.
      for (const postId of desiredPostSubs) {
        socket!.send(JSON.stringify({ type: 'subscribe_post', post_id: postId }))
      }
      // Application-level keepalive — Drogon also sends WS pings, but a
      // small text echo here keeps NAT mappings warm.
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
      } else if (env?.type === 'comment' && env.comment && typeof env.post_id === 'number') {
        const arr = liveCommentsByPost.value.get(env.post_id) ?? []
        arr.push(env.comment as Comment)
        liveCommentsByPost.value.set(env.post_id, arr)
      }
    }

    socket.onclose = () => {
      const wasConnected = connected.value
      connected.value = false
      if (keepalive) { clearInterval(keepalive); keepalive = null }
      if (manualClose || !auth.isAuthed) return

      // A close that follows a successful open is an ordinary drop (server
      // restart, network blip) and resets the failure budget. A close that
      // never opened is a failed handshake and counts against it.
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

  // Record the intent first, then send if we can. onopen replays the set,
  // so a view that mounts before the handshake finishes still ends up
  // subscribed instead of silently missing every live comment.
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

  return {
    conversations,
    connected,
    liveUnavailable,
    totalUnread,
    liveCommentsByPost,
    refreshInbox,
    openConversation,
    send,
    connectSocket,
    disconnectSocket,
    subscribePost,
    unsubscribePost,
    clear,
  }
})
