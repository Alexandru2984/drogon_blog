<script setup lang="ts">
import { computed, nextTick, onMounted, ref, watch } from 'vue'
import { useRoute, useRouter } from 'vue-router'
import { useAuthStore }     from '@/stores/auth'
import { useMessagesStore } from '@/stores/messages'

const auth     = useAuthStore()
const messages = useMessagesStore()
const route    = useRoute()
const router   = useRouter()

const draft = ref('')
const listRef = ref<HTMLElement | null>(null)
const loadingInbox = ref(true)

const selectedId = computed(() => {
  const raw = route.query.peer
  return typeof raw === 'string' ? Number(raw) : null
})

const conversationList = computed(() =>
  Array.from(messages.conversations.values())
    .map(c => ({
      ...c,
      last:    c.messages.length ? c.messages[c.messages.length - 1] : null,
    }))
    .sort((a, b) => (b.last?.id ?? 0) - (a.last?.id ?? 0))
)

const activeConversation = computed(() => {
  if (selectedId.value == null) return null
  return messages.conversations.get(selectedId.value) ?? null
})

function pickPeer(peerId: number) {
  router.replace({ name: 'messages', query: { peer: String(peerId) } })
}

// Mobile shows one pane at a time, so "back" means dropping the selection
// rather than a history pop — the user may have arrived here deep-linked.
function backToList() {
  router.replace({ name: 'messages' })
}

async function sendDraft() {
  if (!selectedId.value) return
  const text = draft.value
  draft.value = ''
  await messages.send(selectedId.value, text)
  await nextTick()
  scrollToBottom()
}

function scrollToBottom() {
  const el = listRef.value
  if (el) el.scrollTop = el.scrollHeight
}

function fmtTime(s: string) {
  return s ? new Date(s.replace(' ', 'T') + 'Z').toLocaleString() : ''
}
function isoTime(s: string) {
  const d = new Date(s.replace(' ', 'T') + 'Z')
  return isNaN(d.getTime()) ? '' : d.toISOString()
}

onMounted(async () => {
  if (!auth.isAuthed) { loadingInbox.value = false; return }
  messages.connectSocket()
  try {
    await messages.refreshInbox()
  } finally {
    loadingInbox.value = false
  }
  if (selectedId.value != null) {
    await messages.openConversation(selectedId.value)
    await nextTick(); scrollToBottom()
  }
})

watch(selectedId, async (id) => {
  if (id == null) return
  await messages.openConversation(id)
  await nextTick(); scrollToBottom()
})

watch(
  () => activeConversation.value?.messages.length,
  () => { nextTick(scrollToBottom) }
)
</script>

<template>
  <div v-if="!auth.isAuthed" class="empty-state">
    <span class="emoji" aria-hidden="true">✉️</span>
    <p>Sign in to read and send messages.</p>
    <router-link to="/login" class="btn">Log in</router-link>
  </div>

  <!-- has-selection drives the mobile single-pane view: below the breakpoint
       exactly one of the two panes is shown, so the reader is not made to
       scroll past the whole conversation list to reach the messages. -->
  <div v-else class="messages-layout" :class="{ 'has-selection': selectedId != null }">
    <aside class="card mlist" aria-label="Conversations">
      <header class="row tight mlist-head">
        <h1 class="mlist-title">Conversations</h1>
        <span class="spacer"></span>
        <span class="conn" :class="{ live: messages.connected }">
          <span aria-hidden="true">{{ messages.connected ? '●' : '○' }}</span>
          {{ messages.connected ? 'Live' : messages.liveUnavailable ? 'No live updates' : 'Connecting…' }}
        </span>
      </header>

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
        <p>No conversations yet. Open someone's profile to start one.</p>
      </div>

      <button
        v-for="c in conversationList"
        :key="c.peer.id"
        class="ghost mlist-item"
        :class="{ active: c.peer.id === selectedId }"
        :aria-current="c.peer.id === selectedId ? 'true' : undefined"
        @click="pickPeer(c.peer.id)"
      >
        <span class="avatar"
              :style="c.peer.profile_image ? `background-image: url(${c.peer.profile_image})` : ''"
              aria-hidden="true"></span>
        <span class="mlist-meta">
          <span class="mlist-name">
            {{ c.peer.username ?? `User #${c.peer.id}` }}
            <span v-if="c.unread" class="badge accent">{{ c.unread }}</span>
          </span>
          <span class="mlist-preview muted">{{ c.last?.content ?? '' }}</span>
        </span>
      </button>
    </aside>

    <section class="card mchat" aria-label="Conversation">
      <template v-if="activeConversation">
        <header class="row tight mchat-head">
          <button class="quiet sm mchat-back" @click="backToList">
            <span aria-hidden="true">←</span> Back
          </button>
          <span class="avatar"
                :style="activeConversation.peer.profile_image ? `background-image: url(${activeConversation.peer.profile_image})` : ''"
                aria-hidden="true"></span>
          <strong class="mchat-peer">
            {{ activeConversation.peer.username ?? `User #${activeConversation.peer.id}` }}
          </strong>
        </header>

        <div ref="listRef" class="mchat-list" role="log" aria-live="polite">
          <div
            v-for="m in activeConversation.messages"
            :key="m.id"
            class="mbubble"
            :class="{ mine: m.sender_id === auth.user!.id }"
          >
            <div class="mbubble-content">{{ m.content }}</div>
            <time v-if="isoTime(m.created_at)" :datetime="isoTime(m.created_at)" class="mbubble-time">
              {{ fmtTime(m.created_at) }}
            </time>
          </div>
        </div>

        <form class="mchat-input" @submit.prevent="sendDraft">
          <label for="msg-draft" class="visually-hidden">Write a message</label>
          <input
            id="msg-draft"
            v-model="draft"
            type="text"
            placeholder="Write a message…"
            maxlength="2000"
            autocomplete="off"
            enterkeyhint="send"
          />
          <button :disabled="!draft.trim()">Send</button>
        </form>
      </template>

      <div v-else class="empty-state">
        <span class="emoji" aria-hidden="true">💬</span>
        <p>Pick a conversation to start chatting.</p>
      </div>
    </section>
  </div>
</template>

<style scoped>
.messages-layout {
  display: grid;
  grid-template-columns: minmax(0, 18rem) minmax(0, 1fr);
  gap: var(--sp-4);
  align-items: start;
}

.mlist {
  padding: var(--sp-3);
  /* Fill the viewport below the sticky navbar rather than a bare 70vh, so
     the two panes line up and neither invents its own scrollbar. */
  max-height: calc(100dvh - var(--nav-h) - var(--sp-6));
  overflow-y: auto;
}
.mlist-head { margin-bottom: var(--sp-2); }
.mlist-title { font-size: var(--step-0); font-weight: 600; margin: 0; }
.mlist-empty { padding: var(--sp-5) var(--sp-3); }
.mlist-skeleton { padding: var(--sp-2) var(--sp-3); }

.conn { font-size: var(--step--1); color: var(--text-faint); white-space: nowrap; }
.conn.live { color: var(--ok); }

.mlist-item {
  width: 100%;
  text-align: left;
  display: flex;
  align-items: center;
  gap: var(--sp-3);
  padding: var(--sp-2) var(--sp-3);
  margin: 0.15rem 0;
  background: transparent;
  border: 1px solid transparent;
  color: var(--text);
}
.mlist-item:hover { background: var(--bg-elev2); }
.mlist-item.active {
  background: var(--bg-elev2);
  border-color: var(--border);
}
.mlist-meta { flex: 1; min-width: 0; display: flex; flex-direction: column; gap: 0.1em; }
.mlist-name { display: flex; gap: 0.4em; align-items: center; font-weight: 550; }
.mlist-preview {
  font-size: var(--step--1);
  overflow: hidden;
  white-space: nowrap;
  text-overflow: ellipsis;
}

.mchat {
  display: flex;
  flex-direction: column;
  min-height: 26rem;
  max-height: calc(100dvh - var(--nav-h) - var(--sp-6));
}
.mchat-head { margin-bottom: var(--sp-3); }
.mchat-peer { min-width: 0; overflow-wrap: anywhere; }
/* Only meaningful in the single-pane mobile layout. */
.mchat-back { display: none; }

.mchat-list {
  flex: 1;
  overflow-y: auto;
  display: flex;
  flex-direction: column;
  gap: var(--sp-2);
  padding: var(--sp-2) 0;
}
.mbubble {
  align-self: flex-start;
  max-width: min(70%, 32rem);
  background: var(--bg-elev2);
  border: 1px solid var(--border);
  border-radius: var(--radius);
  padding: 0.5em 0.75em;
}
.mbubble.mine {
  align-self: flex-end;
  background: var(--accent);
  color: var(--accent-text);
  border-color: transparent;
}
.mbubble-content { white-space: pre-wrap; overflow-wrap: anywhere; }
.mbubble-time {
  display: block;
  font-size: 0.7em;
  margin-top: 0.25em;
  color: var(--text-dim);
}
.mbubble.mine .mbubble-time { color: var(--accent-text); opacity: 0.75; }

.mchat-input {
  display: flex;
  gap: var(--sp-2);
  margin-top: var(--sp-3);
}
.mchat-input input { flex: 1; min-width: 0; }

/* Single pane below the app's own breakpoint. Keeping this aligned with the
   shell prevents a tablet from getting a mobile drawer/tab bar around a
   squeezed two-column conversation layout. */
@media (max-width: 70rem) {
  .messages-layout { grid-template-columns: minmax(0, 1fr); }

  .mlist, .mchat {
    /* The fixed bottom tab bar covers the last ~4rem of the viewport, and
       both panes previously ran underneath it. */
    max-height: calc(100dvh - var(--nav-h) - var(--tabbar-h) - var(--sp-5));
  }
  .mchat { min-height: calc(100dvh - var(--nav-h) - var(--tabbar-h) - var(--sp-5)); }
  .mchat-back { display: inline-flex; }

  .messages-layout.has-selection .mlist { display: none; }
  .messages-layout:not(.has-selection) .mchat { display: none; }
}
</style>
