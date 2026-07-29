<script setup lang="ts">
import { computed, ref } from 'vue'
import type { Comment } from '@/api/comments'

// Nesting is built here rather than sent by the server: the API returns a
// flat list with parent_id, which keeps its ETag a function of the contents
// rather than of the tree shape, and lets a reply arriving over the
// WebSocket slot into place without refetching the structure.
const props = defineProps<{
  comments: Comment[]
  canReply: boolean
  replyingTo: number | null
  posting: boolean
}>()

const emit = defineEmits<{
  (e: 'reply', parentId: number | null): void
  (e: 'submit', payload: { parentId: number; content: string }): void
}>()

interface Node extends Comment {
  children: Node[]
  depth: number
}

// Depth is capped rather than unbounded. Past a few levels the indent eats
// the whole width of a phone, and the thing being replied to is far enough
// up the page that the nesting has stopped conveying it anyway. Deeper
// replies are still shown — they just stop indenting further.
const MAX_DEPTH = 4

const tree = computed<Node[]>(() => {
  const byId = new Map<number, Node>()
  const roots: Node[] = []

  // The list is ordered by id, so a parent is always seen before its
  // children and one pass is enough.
  for (const c of props.comments) {
    byId.set(c.id, { ...c, children: [], depth: 0 })
  }
  for (const c of props.comments) {
    const node = byId.get(c.id)!
    const parent = c.parent_id != null ? byId.get(c.parent_id) : undefined
    if (parent) {
      node.depth = Math.min(parent.depth + 1, MAX_DEPTH)
      parent.children.push(node)
    } else {
      // A reply whose parent is missing from this list (hidden by a
      // moderator, say) is shown at the top level rather than dropped —
      // losing it entirely would silently rewrite the conversation.
      roots.push(node)
    }
  }
  return roots
})

// Flatten for rendering: a recursive component would need a separate file
// and gains nothing here, since depth is already computed and capped.
const flat = computed<Node[]>(() => {
  const out: Node[] = []
  const walk = (nodes: Node[]) => {
    for (const n of nodes) { out.push(n); walk(n.children) }
  }
  walk(tree.value)
  return out
})

const draft = ref('')

function startReply(id: number) {
  draft.value = ''
  emit('reply', id)
}

function send(parentId: number) {
  const text = draft.value.trim()
  if (!text) return
  emit('submit', { parentId, content: text })
  draft.value = ''
}

function when(s: string) {
  return s ? new Date(s.replace(' ', 'T') + 'Z').toLocaleString() : ''
}
function iso(s: string) {
  const d = new Date(s.replace(' ', 'T') + 'Z')
  return isNaN(d.getTime()) ? '' : d.toISOString()
}
</script>

<template>
  <div class="thread">
    <article
      v-for="c in flat"
      :key="c.id"
      class="card comment"
      :style="{ marginLeft: `calc(${c.depth} * var(--thread-indent))` }"
    >
      <header class="row tight comment-head">
        <strong v-if="c.author">{{ c.author.username }}</strong>
        <span v-else class="muted">unknown</span>
        <time v-if="iso(c.created_at)" :datetime="iso(c.created_at)" class="muted">
          {{ when(c.created_at) }}
        </time>
      </header>

      <p class="post-content comment-body">{{ c.content }}</p>

      <div v-if="canReply" class="row tight">
        <button
          v-if="replyingTo !== c.id"
          class="quiet sm"
          @click="startReply(c.id)"
        >Reply</button>
      </div>

      <form v-if="replyingTo === c.id" class="reply-form" @submit.prevent="send(c.id)">
        <label :for="`reply-${c.id}`" class="visually-hidden">
          Reply to {{ c.author?.username ?? 'this comment' }}
        </label>
        <textarea
          :id="`reply-${c.id}`"
          v-model="draft"
          rows="3"
          maxlength="2000"
          placeholder="Write a reply…"
        ></textarea>
        <div class="row tight" style="margin-top: var(--sp-2);">
          <button class="sm" :disabled="!draft.trim() || posting">
            {{ posting ? 'Posting…' : 'Reply' }}
          </button>
          <button type="button" class="quiet sm" @click="emit('reply', null)">Cancel</button>
        </div>
      </form>
    </article>
  </div>
</template>

<style scoped>
.thread {
  /* One knob for the indent, halved on a phone where the content column is
     narrow enough that a full step per level would squeeze the text to a
     couple of words per line by the third reply. */
  --thread-indent: 1.75rem;
}
@media (max-width: 48rem) {
  .thread { --thread-indent: 0.9rem; }
}

.comment { margin-top: var(--sp-3); }
.comment-head { margin-bottom: var(--sp-2); }
.comment-head time { font-size: 0.78rem; }
.comment-body { margin: 0 0 var(--sp-2); }

.reply-form { margin-top: var(--sp-3); }
.reply-form textarea { min-height: 5rem; }
</style>
