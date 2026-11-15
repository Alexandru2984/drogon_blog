<script setup lang="ts">
import { formatDateTime, isoDate as toIsoDate, parseApiDate } from '@/lib/date'
import { computed, ref } from 'vue'
import type { Comment } from '@/api/comments'
import { useAuthStore } from '@/stores/auth'
import Avatar from '@/components/Avatar.vue'
import ReportButton from '@/components/ReportButton.vue'

const auth = useAuthStore()

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
  (e: 'edit', payload: { id: number; content: string }): void
  (e: 'remove', id: number): void
}>()

interface Node extends Comment {
  children: Node[]
  depth: number
}

const MAX_DEPTH = 4

const tree = computed<Node[]>(() => {
  const byId = new Map<number, Node>()
  const roots: Node[] = []

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
      roots.push(node)
    }
  }
  return roots
})

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

function isMine(c: Comment) {
  return auth.isAuthed && !c.deleted && c.author?.id === auth.user!.id
}

const editingId = ref<number | null>(null)
const editDraft = ref('')

function startEdit(c: Comment) {
  editingId.value = c.id
  editDraft.value = c.content
}

function saveEdit(id: number) {
  const text = editDraft.value.trim()
  if (!text) return
  emit('edit', { id, content: text })
  editingId.value = null
}

function remove(c: Comment) {
  const hasReplies = props.comments.some(o => o.parent_id === c.id)
  const msg = hasReplies
    ? 'Delete this comment? It has replies, so it will stay in the thread as "deleted".'
    : 'Delete this comment?'
  if (confirm(msg)) emit('remove', c.id)
}

function isEdited(c: Comment): boolean {
  if (c.deleted || !c.updated_at) return false
  const created = parseApiDate(c.created_at)
  const updated = parseApiDate(c.updated_at)
  return !!created && !!updated && updated.getTime() - created.getTime() > 5_000
}

function when(s: string) { return formatDateTime(s) }
function iso(s: string) { return toIsoDate(s) }
</script>

<template>
  <div class="thread">
    <article
      v-for="c in flat"
      :id="`comment-${c.id}`"
      :key="c.id"
      class="card comment"
      :style="{ marginLeft: `calc(${c.depth} * var(--thread-indent))` }"
    >
      <header class="row tight comment-head">
        <Avatar
          size="sm"
          :image-url="c.author?.profile_image"
          :username="c.author?.username"
        />
        <router-link
          v-if="c.author"
          :to="{ name: 'profile', params: { id: c.author.id } }"
          class="comment-author meta-link"
        >{{ c.author.username }}</router-link>
        <span v-else class="muted">{{ c.deleted ? 'deleted' : 'unknown' }}</span>
        <time v-if="iso(c.created_at)" :datetime="iso(c.created_at)" class="muted">
          {{ when(c.created_at) }}
        </time>
        <span v-if="isEdited(c)" class="faint comment-edited" :title="when(c.updated_at!)">
          (edited)
        </span>
        <span class="spacer"></span>
        <a :href="`#comment-${c.id}`" class="faint comment-anchor" :aria-label="`Link to comment ${c.id}`">#</a>
      </header>

      <p v-if="c.deleted" class="muted comment-body comment-deleted">This comment was deleted.</p>
      <form v-else-if="editingId === c.id" class="reply-form" @submit.prevent="saveEdit(c.id)">
        <label :for="`edit-${c.id}`" class="visually-hidden">Edit your comment</label>
        <textarea :id="`edit-${c.id}`" v-model="editDraft" rows="3" maxlength="2000"></textarea>
        <div class="row tight" style="margin-top: var(--sp-2);">
          <button class="sm" :disabled="!editDraft.trim() || posting">
            {{ posting ? 'Saving…' : 'Save' }}
          </button>
          <button type="button" class="quiet sm" @click="editingId = null">Cancel</button>
        </div>
      </form>
      <p v-else class="post-content comment-body">{{ c.content }}</p>

      <div v-if="auth.isAuthed && !c.deleted && editingId !== c.id" class="row tight comment-actions">
        <button
          v-if="canReply && replyingTo !== c.id"
          class="quiet sm"
          @click="startReply(c.id)"
        >Reply</button>
        <template v-if="isMine(c)">
          <button class="quiet sm" :disabled="posting" @click="startEdit(c)">Edit</button>
          <button class="quiet sm danger-text" :disabled="posting" @click="remove(c)">Delete</button>
        </template>
        <ReportButton
          v-else-if="c.author"
          target-type="comment"
          :target-id="c.id"
        />
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
  --thread-indent: 1.75rem;
}
@media (max-width: 48rem) {
  .thread { --thread-indent: 0.9rem; }
}

.comment { margin-top: var(--sp-3); scroll-margin-top: calc(var(--nav-h, 3.5rem) + 1rem); }
.comment:target {
  border-color: var(--accent);
  box-shadow: 0 0 0 2px color-mix(in srgb, var(--accent) 22%, transparent);
}
.comment-head { margin-bottom: var(--sp-2); flex-wrap: wrap; }
.comment-author { font-weight: 600; }
.comment-head time { font-size: 0.78rem; }
.comment-edited { font-size: 0.72rem; }
.comment-anchor { font-size: 0.8rem; text-decoration: none; padding: 0 0.25rem; }
.comment-anchor:hover { color: var(--accent); }
.comment-body { margin: 0 0 var(--sp-2); }
.comment-actions { flex-wrap: wrap; }

.reply-form { margin-top: var(--sp-3); }
.reply-form textarea { min-height: 5rem; }

.comment-deleted { font-style: italic; }

.danger-text { color: var(--danger); }
.danger-text:hover { background: var(--danger-soft); color: var(--danger); }
</style>
