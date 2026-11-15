<script setup lang="ts">
import Avatar from '@/components/Avatar.vue'
import { formatDateTime, isoDate as toIsoDate, parseApiDate } from '@/lib/date'
import { computed, nextTick, onBeforeUnmount, onMounted, ref, watch } from 'vue'
import { useRoute, useRouter } from 'vue-router'
import { useAuthStore }     from '@/stores/auth'
import { useMessagesStore } from '@/stores/messages'
import { useToastStore }    from '@/stores/toast'
import { usersApi, type UserSummary } from '@/api/users'
import type { MessageRow } from '@/api/messages'

const auth     = useAuthStore()
const messages = useMessagesStore()
const toasts   = useToastStore()
const route    = useRoute()
const router   = useRouter()

const draft        = ref('')
const listRef      = ref<HTMLElement | null>(null)
const composerRef  = ref<HTMLTextAreaElement | null>(null)
const loadingInbox = ref(true)
const sending      = ref(false)
const loadingOlder = ref(false)
const filter       = ref('')
const MAX_LEN = 2000

// `peer` is the canonical query key; `user` is accepted so profile pages
// and external links can say /messages?user=42.
const selectedId = computed(() => {
  const raw = route.query.peer ?? route.query.user
  const n = typeof raw === 'string' ? Number(raw) : NaN
  return Number.isSafeInteger(n) && n > 0 ? n : null
})

const conversationList = computed(() => {
  const f = filter.value.trim().toLowerCase()
  return Array.from(messages.conversations.values())
    .map(c => ({ ...c, last: c.messages.length ? c.messages[c.messages.length - 1] : null }))
    .filter(c => c.last || c.peer.id === selectedId.value)
    .filter(c => !f || (c.peer.username ?? '').toLowerCase().includes(f))
    .sort((a, b) => (b.last?.id ?? Number.MAX_SAFE_INTEGER) - (a.last?.id ?? Number.MAX_SAFE_INTEGER))
})

const activeConversation = computed(() => {
  if (selectedId.value == null) return null
  return messages.conversations.get(selectedId.value) ?? null
})

// Messages grouped under a day separator, so a long thread reads as a
// timeline instead of a wall of identical timestamps.
const grouped = computed(() => {
  const out: Array<{ day: string; items: MessageRow[] }> = []
  for (const m of activeConversation.value?.messages ?? []) {
    const d = parseApiDate(m.created_at)
    const day = d ? dayLabel(d) : ''
    const last = out[out.length - 1]
    if (last && last.day === day) last.items.push(m)
    else out.push({ day, items: [m] })
  }
  return out
})

function dayLabel(d: Date) {
  const today = new Date()
  const y = new Date(); y.setDate(today.getDate() - 1)
  const same = (a: Date, b: Date) => a.toDateString() === b.toDateString()
  if (same(d, today)) return 'Today'
  if (same(d, y)) return 'Yesterday'
  return d.toLocaleDateString(undefined, { weekday: 'short', month: 'short', day: 'numeric', year: d.getFullYear() === today.getFullYear() ? undefined : 'numeric' })
}
function timeOnly(s: string) {
  const d = parseApiDate(s)
  return d ? d.toLocaleTimeString(undefined, { hour: '2-digit', minute: '2-digit' }) : ''
}
function previewTime(s?: string) {
  const d = parseApiDate(s)
  if (!d) return ''
  return d.toDateString() === new Date().toDateString()
    ? d.toLocaleTimeString(undefined, { hour: '2-digit', minute: '2-digit' })
    : d.toLocaleDateString(undefined, { month: 'short', day: 'numeric' })
}

function pickPeer(peerId: number) {
  showPicker.value = false
  router.replace({ name: 'messages', query: { peer: String(peerId) } })
}
function backToList() {
  router.replace({ name: 'messages' })
}

// --- Composer -------------------------------------------------------------
function autoGrow() {
  const el = composerRef.value
  if (!el) return
  el.style.height = 'auto'
  el.style.height = Math.min(el.scrollHeight, 160) + 'px'
}
watch(draft, () => nextTick(autoGrow))

function onComposerKeydown(e: KeyboardEvent) {
  // Enter sends, Shift+Enter is a newline. IME composition (CJK input)
  // also fires Enter to commit a candidate; that must not send.
  if (e.key === 'Enter' && !e.shiftKey && !e.isComposing) {
    e.preventDefault()
    sendDraft()
  }
}

async function sendDraft() {
  if (!selectedId.value || sending.value) return
  const text = draft.value.trim()
  if (!text) return
  sending.value = true
  draft.value = ''
  try {
    await messages.send(selectedId.value, text)
    await nextTick()
    scrollToBottom(true)
  } catch (e: any) {
    draft.value = text // give the text back rather than losing it
    toasts.push(e?.response?.data?.error ?? 'Message not sent', 'error')
  } finally {
    sending.value = false
    composerRef.value?.focus()
  }
}

async function unsend(m: MessageRow) {
  if (!selectedId.value) return
  if (!confirm('Delete this message for everyone?')) return
  try {
    await messages.removeMessage(selectedId.value, m.id)
  } catch (e: any) {
    toasts.push(e?.response?.data?.error ?? 'Could not delete message', 'error')
  }
}

async function copyMessage(m: MessageRow) {
  try {
    await navigator.clipboard.writeText(m.content)
    toasts.push('Copied', 'ok')
  } catch { /* clipboard blocked: ignore */ }
}

// --- Scrolling + history ----------------------------------------------------
const nearBottom = ref(true)
function onListScroll() {
  const el = listRef.value
  if (!el) return
  nearBottom.value = el.scrollHeight - el.scrollTop - el.clientHeight < 80
  if (el.scrollTop < 40) loadOlder()
}
function scrollToBottom(force = false) {
  const el = listRef.value
  if (el && (force || nearBottom.value)) el.scrollTop = el.scrollHeight
}

async function loadOlder() {
  if (!selectedId.value || loadingOlder.value || !activeConversation.value?.nextCursor) return
  const el = listRef.value
  const prevHeight = el?.scrollHeight ?? 0
  loadingOlder.value = true
  try {
    await messages.loadOlderMessages(selectedId.value)
    await nextTick()
    // Keep the message the reader was looking at in place.
    if (el) el.scrollTop = el.scrollHeight - prevHeight + el.scrollTop
  } catch {
    toasts.push('Could not load older messages', 'error')
  } finally {
    loadingOlder.value = false
  }
}

async function openSelected(id: number) {
  try {
    await messages.openConversation(id)
  } catch (e: any) {
    const status = e?.response?.status
    if (status === 404) {
      toasts.push('That user does not exist', 'error')
      backToList()
      return
    }
    // A brand-new conversation still needs the peer's name in the header.
    try {
      const u = await usersApi.get(id)
      messages.upsertPeer({ id: u.id, username: u.username, profile_image: u.profile_image, online: u.online })
    } catch {
      toasts.push('That user does not exist', 'error')
      backToList()
      return
    }
  }
  if (!messages.conversations.get(id)) messages.upsertPeer({ id })
  await nextTick()
  scrollToBottom(true)
  composerRef.value?.focus({ preventScroll: true })
}

// --- New message picker -----------------------------------------------------
const showPicker   = ref(false)
const pickerQuery  = ref('')
const pickerResults = ref<UserSummary[]>([])
const pickerBusy   = ref(false)
const pickerInput  = ref<HTMLInputElement | null>(null)
let pickerTimer: ReturnType<typeof setTimeout> | undefined
let pickerSeq = 0

async function runPicker() {
  const seq = ++pickerSeq
  pickerBusy.value = true
  try {
    const res = await usersApi.search(pickerQuery.value.trim() || undefined, undefined, 20)
    if (seq === pickerSeq) pickerResults.value = res.users
  } catch {
    if (seq === pickerSeq) pickerResults.value = []
  } finally {
    if (seq === pickerSeq) pickerBusy.value = false
  }
}
watch(pickerQuery, () => {
  clearTimeout(pickerTimer)
  pickerTimer = setTimeout(runPicker, 250)
})
async function openPicker() {
  showPicker.value = !showPicker.value
  if (showPicker.value) {
    pickerQuery.value = ''
    runPicker()
    await nextTick()
    pickerInput.value?.focus()
  }
}

// --- Lifecycle --------------------------------------------------------------
onMounted(async () => {
  if (!auth.isAuthed) { loadingInbox.value = false; return }
  messages.connectSocket()
  try {
    await messages.refreshInbox()
  } finally {
    loadingInbox.value = false
  }
  if (route.query.user && !route.query.peer && selectedId.value) {
    router.replace({ name: 'messages', query: { peer: String(selectedId.value) } })
    return // the selectedId watcher is not triggered (same id), so open now:
  }
  if (selectedId.value != null) await openSelected(selectedId.value)
})
// Handles the redirect above as well, since `peer` replaces `user` with the same id.
watch(() => route.query.peer, async (raw, old) => {
  if (raw === old || selectedId.value == null) return
  await openSelected(selectedId.value)
})
onBeforeUnmount(() => clearTimeout(pickerTimer))

watch(
  () => activeConversation.value?.messages.length,
  async (n, old) => {
    await nextTick()
    // New message at the end (not older history at the top) → follow it.
    const msgs = activeConversation.value?.messages ?? []
    const last = msgs[msgs.length - 1]
    const mine = last && auth.user && last.sender_id === auth.user.id
    if ((old ?? 0) < (n ?? 0)) scrollToBottom(!!mine)
    // Incoming message while the thread is open → mark it read right away.
    if (last && !mine && !last.is_read && selectedId.value && document.visibilityState === 'visible') {
      messages.openConversation(selectedId.value).catch(() => {})
    }
  },
)

function fmtTime(s: string) { return formatDateTime(s) }
function isoTime(s: string) { return toIsoDate(s) }
</script>

<template>
  <div v-if="!auth.isAuthed" class="empty-state">
    <span class="emoji" aria-hidden="true">✉️</span>
    <p>Sign in to read and send messages.</p>
    <router-link to="/login" class="btn">Log in</router-link>
  </div>

  <div v-else class="messages-layout" :class="{ 'has-selection': selectedId != null }">
    <aside class="card mlist" aria-label="Conversations">
      <header class="row tight mlist-head">
        <h1 class="mlist-title">Messages</h1>
        <span class="spacer"></span>
        <span class="conn" :class="{ live: messages.connected }">
          <span aria-hidden="true">{{ messages.connected ? '●' : '○' }}</span>
          {{ messages.connected ? 'Live' : messages.liveUnavailable ? 'Offline' : 'Connecting…' }}
        </span>
        <button v-if="messages.liveUnavailable" type="button" class="quiet sm" @click="messages.retryLive()">
          Retry
        </button>
      </header>

      <div class="row tight mlist-tools">
        <label for="conv-filter" class="visually-hidden">Filter conversations</label>
        <input id="conv-filter" v-model="filter" type="search" placeholder="Filter…" autocomplete="off" />
        <button type="button" class="sm new-msg" :aria-expanded="showPicker" aria-controls="new-msg-picker"
                @click="openPicker">✏️ New message</button>
      </div>

      <div v-if="showPicker" id="new-msg-picker" class="picker">
        <label for="picker-q" class="visually-hidden">Search people</label>
        <input id="picker-q" ref="pickerInput" v-model="pickerQuery" type="search"
               placeholder="Search people by username…" autocomplete="off"
               @keydown.esc="showPicker = false" />
        <p v-if="pickerBusy" class="muted picker-note">Searching…</p>
        <p v-else-if="!pickerResults.length" class="muted picker-note">No one found.</p>
        <ul v-else class="picker-list">
          <li v-for="u in pickerResults" :key="u.id">
            <button type="button" class="ghost picker-item" @click="pickPeer(u.id)">
              <span class="avatar-wrap">
                <Avatar size="sm" :image-url="u.profile_image" :username="u.username" />
                <span v-if="u.online" class="online-dot" title="Online"></span>
              </span>
              <span class="picker-name">{{ u.username }}</span>
              <span v-if="u.bio" class="muted picker-bio">{{ u.bio }}</span>
            </button>
          </li>
        </ul>
      </div>

      <div v-if="loadingInbox" aria-hidden="true">
        <div v-for="n in 3" :key="n" class="row tight mlist-skeleton">
          <span class="avatar skeleton"></span>
          <div style="flex: 1;">
            <div class="skeleton line short"></div>
            <div class="skeleton line medium"></div>
          </div>
        </div>
      </div>

      <div v-else-if="!conversationList.length" class="empty-state mlist-empty">
        <p>{{ filter ? 'No conversation matches.' : 'No conversations yet. Start one with “New message”.' }}</p>
      </div>

      <button
        v-for="c in conversationList"
        :key="c.peer.id"
        class="ghost mlist-item"
        :class="{ active: c.peer.id === selectedId, unread: c.unread > 0 }"
        :aria-current="c.peer.id === selectedId ? 'true' : undefined"
        @click="pickPeer(c.peer.id)"
      >
        <span class="avatar-wrap">
          <Avatar :image-url="c.peer.profile_image" :username="c.peer.username" />
          <span v-if="c.peer.online" class="online-dot" title="Online"></span>
        </span>
        <span class="mlist-meta">
          <span class="mlist-name">
            <span class="mlist-username">{{ c.peer.username ?? `User #${c.peer.id}` }}</span>
            <span class="spacer"></span>
            <time v-if="c.last" class="mlist-when faint">{{ previewTime(c.last.created_at) }}</time>
          </span>
          <span class="mlist-preview-row">
            <span class="mlist-preview muted">
              <template v-if="c.last && c.last.sender_id === auth.user!.id">You: </template>{{ c.last?.content ?? 'No messages yet' }}
            </span>
            <span v-if="c.unread" class="badge accent">{{ c.unread }}</span>
          </span>
        </span>
      </button>
    </aside>

    <section class="card mchat" aria-label="Conversation">
      <template v-if="activeConversation">
        <header class="row tight mchat-head">
          <button class="quiet sm mchat-back" @click="backToList">
            <span aria-hidden="true">←</span> Back
          </button>
          <router-link :to="{ name: 'profile', params: { id: activeConversation.peer.id } }" class="mchat-peer-link">
            <span class="avatar-wrap">
              <Avatar :image-url="activeConversation.peer.profile_image" :username="activeConversation.peer.username" />
              <span v-if="activeConversation.peer.online" class="online-dot"></span>
            </span>
            <span class="mchat-peer-text">
              <strong class="mchat-peer">{{ activeConversation.peer.username ?? `User #${activeConversation.peer.id}` }}</strong>
              <span class="faint mchat-status">{{ activeConversation.peer.online ? 'Online' : 'Offline' }}</span>
            </span>
          </router-link>
        </header>

        <div ref="listRef" class="mchat-list" role="log" aria-live="polite" @scroll.passive="onListScroll">
          <div v-if="activeConversation.nextCursor" class="older">
            <button type="button" class="quiet sm" :disabled="loadingOlder" @click="loadOlder">
              {{ loadingOlder ? 'Loading…' : 'Load older messages' }}
            </button>
          </div>
          <p v-if="!activeConversation.messages.length" class="muted mchat-empty">
            Say hi 👋 — this is the start of your conversation.
          </p>
          <template v-for="g in grouped" :key="g.day">
            <div class="day-sep" role="separator"><span>{{ g.day }}</span></div>
            <div
              v-for="m in g.items"
              :key="m.id"
              class="mbubble"
              :class="{ mine: m.sender_id === auth.user!.id }"
            >
              <div class="mbubble-content">{{ m.content }}</div>
              <div class="mbubble-foot">
                <time v-if="isoTime(m.created_at)" :datetime="isoTime(m.created_at)"
                      :title="fmtTime(m.created_at)" class="mbubble-time">{{ timeOnly(m.created_at) }}</time>
                <span v-if="m.sender_id === auth.user!.id" class="receipt"
                      :title="m.is_read ? 'Read' : 'Sent'" :aria-label="m.is_read ? 'Read' : 'Sent'">
                  {{ m.is_read ? '✓✓' : '✓' }}
                </span>
              </div>
              <div class="mbubble-actions">
                <button type="button" class="mb-act" title="Copy" aria-label="Copy message" @click="copyMessage(m)">⧉</button>
                <button v-if="m.sender_id === auth.user!.id" type="button" class="mb-act"
                        title="Delete for everyone" aria-label="Delete message" @click="unsend(m)">🗑</button>
              </div>
            </div>
          </template>
        </div>

        <form class="mchat-input" @submit.prevent="sendDraft">
          <label for="msg-draft" class="visually-hidden">Write a message</label>
          <textarea
            id="msg-draft"
            ref="composerRef"
            v-model="draft"
            rows="1"
            placeholder="Write a message…"
            :maxlength="MAX_LEN"
            autocomplete="off"
            enterkeyhint="send"
            @keydown="onComposerKeydown"
          ></textarea>
          <button :disabled="!draft.trim() || sending">Send</button>
        </form>
        <p v-if="draft.length > MAX_LEN - 200" class="faint char-left">{{ MAX_LEN - draft.length }} characters left</p>
      </template>

      <div v-else class="empty-state">
        <span class="emoji" aria-hidden="true">💬</span>
        <p>Pick a conversation, or start a new one.</p>
        <button type="button" @click="openPicker">✏️ New message</button>
      </div>
    </section>
  </div>
</template>

<style scoped>
.messages-layout {
  display: grid;
  grid-template-columns: minmax(0, 20rem) minmax(0, 1fr);
  gap: var(--sp-4);
  align-items: start;
}

.mlist {
  padding: var(--sp-3);
  max-height: calc(100dvh - var(--nav-h) - var(--sp-6));
  overflow-y: auto;
}
.mlist-head { margin-bottom: var(--sp-2); }
.mlist-title { font-size: var(--step-0); font-weight: 650; margin: 0; }
.mlist-tools { margin-bottom: var(--sp-2); flex-wrap: nowrap; }
.mlist-tools input { flex: 1; min-width: 0; padding-block: 0.35rem; }
.new-msg { white-space: nowrap; }
.mlist-empty { padding: var(--sp-5) var(--sp-3); }
.mlist-skeleton { padding: var(--sp-2) var(--sp-3); }

.conn { font-size: var(--step--1); color: var(--text-faint); white-space: nowrap; }
.conn.live { color: var(--ok); }

.picker {
  border: 1px solid var(--border); border-radius: var(--radius);
  padding: var(--sp-2); margin-bottom: var(--sp-3); background: var(--bg-inset);
}
.picker-note { margin: var(--sp-2) var(--sp-1) 0; font-size: var(--step--1); }
.picker-list { list-style: none; margin: var(--sp-2) 0 0; padding: 0; max-height: 16rem; overflow-y: auto; }
.picker-item {
  width: 100%; display: flex; align-items: center; gap: var(--sp-2);
  text-align: left; padding: var(--sp-2); background: transparent; border: 0; color: var(--text);
}
.picker-item:hover { background: var(--bg-elev2); }
.picker-name { font-weight: 600; }
.picker-bio { font-size: 0.78rem; overflow: hidden; white-space: nowrap; text-overflow: ellipsis; min-width: 0; flex: 1; }

.avatar-wrap { position: relative; display: inline-flex; flex: 0 0 auto; }
.online-dot {
  position: absolute; right: -1px; bottom: -1px; width: 0.7rem; height: 0.7rem;
  border-radius: 50%; background: var(--ok); border: 2px solid var(--bg-elev);
}

.mlist-item {
  width: 100%; text-align: left; display: flex; align-items: center; gap: var(--sp-3);
  padding: var(--sp-2) var(--sp-3); margin: 0.15rem 0;
  background: transparent; border: 1px solid transparent; color: var(--text);
}
.mlist-item:hover { background: var(--bg-elev2); }
.mlist-item.active { background: var(--bg-elev2); border-color: var(--border); }
.mlist-item.unread .mlist-username, .mlist-item.unread .mlist-preview { font-weight: 700; color: var(--text); }
.mlist-meta { flex: 1; min-width: 0; display: flex; flex-direction: column; gap: 0.1em; }
.mlist-name { display: flex; gap: 0.4em; align-items: baseline; font-weight: 550; min-width: 0; }
.mlist-username { overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.mlist-when { font-size: 0.72rem; white-space: nowrap; }
.mlist-preview-row { display: flex; gap: var(--sp-2); align-items: center; min-width: 0; }
.mlist-preview {
  flex: 1; font-size: var(--step--1);
  overflow: hidden; white-space: nowrap; text-overflow: ellipsis;
}

.mchat {
  display: flex; flex-direction: column;
  min-height: 26rem;
  height: calc(100dvh - var(--nav-h) - var(--sp-6));
}
.mchat-head { margin-bottom: var(--sp-2); padding-bottom: var(--sp-2); border-bottom: 1px solid var(--border-soft); }
.mchat-peer-link { display: flex; align-items: center; gap: var(--sp-2); color: var(--text); min-width: 0; }
.mchat-peer-link:hover { text-decoration: none; }
.mchat-peer-text { display: flex; flex-direction: column; line-height: 1.2; min-width: 0; }
.mchat-peer { overflow-wrap: anywhere; }
.mchat-status { font-size: 0.72rem; }
.mchat-back { display: none; }

.mchat-list {
  flex: 1; overflow-y: auto; overscroll-behavior: contain;
  display: flex; flex-direction: column; gap: var(--sp-2);
  padding: var(--sp-2) var(--sp-1);
}
.older { display: flex; justify-content: center; }
.mchat-empty { text-align: center; margin: auto 0; }
.day-sep {
  display: flex; align-items: center; gap: var(--sp-2);
  font-size: 0.72rem; color: var(--text-faint); margin: var(--sp-2) 0;
}
.day-sep::before, .day-sep::after { content: ''; flex: 1; height: 1px; background: var(--border-soft); }

.mbubble {
  position: relative; align-self: flex-start; max-width: min(78%, 34rem);
  background: var(--bg-elev2); border: 1px solid var(--border);
  border-radius: 1rem 1rem 1rem 0.3rem; padding: 0.45em 0.8em;
}
.mbubble.mine {
  align-self: flex-end; background: var(--accent); color: var(--accent-text);
  border-color: transparent; border-radius: 1rem 1rem 0.3rem 1rem;
}
.mbubble-content { white-space: pre-wrap; overflow-wrap: anywhere; }
.mbubble-foot { display: flex; justify-content: flex-end; align-items: center; gap: 0.35em; margin-top: 0.15em; }
.mbubble-time { font-size: 0.68em; color: var(--text-dim); }
.mbubble.mine .mbubble-time, .receipt { color: var(--accent-text); opacity: 0.8; }
.receipt { font-size: 0.68em; letter-spacing: -0.15em; }

.mbubble-actions {
  position: absolute; top: -0.9rem; display: flex; gap: 2px;
  opacity: 0; pointer-events: none; transition: opacity 120ms;
}
.mbubble:not(.mine) .mbubble-actions { right: -0.4rem; }
.mbubble.mine .mbubble-actions { left: -0.4rem; }
.mbubble:hover .mbubble-actions, .mbubble:focus-within .mbubble-actions { opacity: 1; pointer-events: auto; }
@media (hover: none) { .mbubble-actions { position: static; opacity: 1; pointer-events: auto; justify-content: flex-end; margin-top: 0.2em; } }
.mb-act {
  padding: 0.1rem 0.4rem; font-size: 0.75rem; min-height: 0;
  background: var(--bg-elev); color: var(--text-dim);
  border: 1px solid var(--border); border-radius: var(--radius-sm); box-shadow: none;
}

.mchat-input { display: flex; gap: var(--sp-2); margin-top: var(--sp-2); align-items: flex-end; }
.mchat-input textarea {
  flex: 1; min-width: 0; resize: none; min-height: 2.6rem; max-height: 10rem;
  line-height: 1.4; padding-block: 0.6rem; border-radius: 1.3rem;
}
.mchat-input button { border-radius: 1.3rem; }
.char-left { margin: var(--sp-1) 0 0; font-size: 0.72rem; text-align: right; }

@media (max-width: 70rem) {
  .messages-layout { grid-template-columns: minmax(0, 1fr); }
  .mlist {
    max-height: calc(100dvh - var(--nav-h) - var(--tabbar-h) - env(safe-area-inset-bottom) - var(--sp-5));
  }
  .mchat {
    height: calc(100dvh - var(--nav-h) - var(--tabbar-h) - env(safe-area-inset-bottom) - var(--sp-5));
    min-height: 20rem;
  }
  .mchat-back { display: inline-flex; }
  .messages-layout.has-selection .mlist { display: none; }
  .messages-layout:not(.has-selection) .mchat { display: none; }
}
</style>
