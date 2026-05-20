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

onMounted(async () => {
  if (!auth.isAuthed) return
  messages.connectSocket()
  await messages.refreshInbox()
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
  <p v-if="!auth.isAuthed" class="muted">
    <router-link to="/login">Log in</router-link> to view your messages.
  </p>

  <div v-else class="messages-layout">
    <aside class="card mlist">
      <header class="toolbar" style="margin-bottom: 0.5rem;">
        <strong>Conversations</strong>
        <span class="spacer"></span>
        <span class="muted" style="font-size: 0.8em;">
          {{ messages.connected ? '● live' : '○ offline' }}
        </span>
      </header>

      <p v-if="!conversationList.length" class="muted">
        No conversations yet.
      </p>

      <button
        v-for="c in conversationList"
        :key="c.peer.id"
        class="ghost mlist-item"
        :class="{ active: c.peer.id === selectedId }"
        @click="pickPeer(c.peer.id)"
      >
        <span class="avatar"
              :style="c.peer.profile_image ? `background-image: url(${c.peer.profile_image})` : ''"></span>
        <div class="mlist-meta">
          <div class="mlist-name">
            {{ c.peer.username ?? `User #${c.peer.id}` }}
            <span v-if="c.unread" class="badge">{{ c.unread }}</span>
          </div>
          <div class="mlist-preview muted">
            {{ c.last?.content ?? '' }}
          </div>
        </div>
      </button>
    </aside>

    <section class="card mchat">
      <template v-if="activeConversation">
        <header class="toolbar" style="margin-bottom: 0.75rem;">
          <span class="avatar"
                :style="activeConversation.peer.profile_image ? `background-image: url(${activeConversation.peer.profile_image})` : ''"></span>
          <strong>{{ activeConversation.peer.username ?? `User #${activeConversation.peer.id}` }}</strong>
        </header>

        <div ref="listRef" class="mchat-list">
          <div
            v-for="m in activeConversation.messages"
            :key="m.id"
            class="mbubble"
            :class="{ mine: m.sender_id === auth.user!.id }"
          >
            <div class="mbubble-content">{{ m.content }}</div>
            <div class="mbubble-time muted">{{ fmtTime(m.created_at) }}</div>
          </div>
        </div>

        <form class="mchat-input" @submit.prevent="sendDraft">
          <input
            v-model="draft"
            type="text"
            placeholder="Write a message…"
            maxlength="2000"
            autocomplete="off"
          />
          <button :disabled="!draft.trim()">Send</button>
        </form>
      </template>

      <p v-else class="muted">Pick a conversation to start chatting.</p>
    </section>
  </div>
</template>

<style scoped>
.messages-layout {
  display: grid;
  grid-template-columns: 280px 1fr;
  gap: 1rem;
}
.mlist {
  padding: 0.75rem;
  max-height: 70vh;
  overflow-y: auto;
}
.mlist-item {
  width: 100%;
  text-align: left;
  display: flex;
  align-items: center;
  gap: 0.6rem;
  padding: 0.5rem 0.6rem;
  margin: 0.15rem 0;
  background: transparent;
  border: 1px solid transparent;
}
.mlist-item.active {
  background: var(--bg-elev2);
  border-color: var(--border);
}
.mlist-meta { flex: 1; min-width: 0; }
.mlist-name { display: flex; gap: 0.4em; align-items: center; }
.mlist-preview {
  font-size: 0.85em;
  overflow: hidden;
  white-space: nowrap;
  text-overflow: ellipsis;
}
.badge {
  background: var(--accent);
  color: white;
  border-radius: 999px;
  font-size: 0.7em;
  padding: 0 0.45em;
  min-width: 1.4em;
  text-align: center;
}
.mchat {
  display: flex;
  flex-direction: column;
  min-height: 70vh;
}
.mchat-list {
  flex: 1;
  overflow-y: auto;
  display: flex;
  flex-direction: column;
  gap: 0.5rem;
  padding: 0.5rem 0;
}
.mbubble {
  align-self: flex-start;
  max-width: 70%;
  background: var(--bg-elev2);
  border: 1px solid var(--border);
  border-radius: var(--radius);
  padding: 0.5em 0.75em;
}
.mbubble.mine {
  align-self: flex-end;
  background: var(--accent);
  color: #fff;
  border-color: transparent;
}
.mbubble-content { white-space: pre-wrap; word-break: break-word; }
.mbubble-time { font-size: 0.7em; margin-top: 0.25em; }
.mbubble.mine .mbubble-time { color: rgba(255,255,255,0.7); }
.mchat-input {
  display: flex;
  gap: 0.5rem;
  margin-top: 0.75rem;
}
.mchat-input input { flex: 1; }

@media (max-width: 720px) {
  .messages-layout { grid-template-columns: 1fr; }
  .mlist { max-height: 30vh; }
}
</style>
