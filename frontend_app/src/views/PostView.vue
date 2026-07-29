<script setup lang="ts">
import { ref, computed, onBeforeUnmount, onMounted, watch } from 'vue'
import { useRouter } from 'vue-router'
import { postsApi, type Post } from '@/api/posts'
import { commentsApi, type Comment } from '@/api/comments'
import { useAuthStore } from '@/stores/auth'
import { useMessagesStore } from '@/stores/messages'
import { useToastStore } from '@/stores/toast'
import { sanitizePostHtml } from '@/lib/sanitize'

const props = defineProps<{ id: number }>()

const post = ref<Post | null>(null)
const likes = ref<number>(0)
const liked = ref(false)
const likeBusy = ref(false)
const comments = ref<Comment[]>([])
const commentsLoading = ref(true)
const newComment = ref('')
const posting = ref(false)
const loading = ref(true)
const error = ref('')

const auth     = useAuthStore()
const router   = useRouter()
const toasts   = useToastStore()
const live     = useMessagesStore()

const isOwner = computed(() => auth.isAuthed && post.value?.author?.id === auth.user!.id)

const bodyHtml = computed(() =>
  post.value?.content_html ? sanitizePostHtml(post.value.content_html) : '')

async function load() {
  loading.value = true
  commentsLoading.value = true
  error.value = ''
  try {
    const [p, l, c] = await Promise.all([
      postsApi.get(props.id),
      postsApi.likesCount(props.id).catch(() => ({ likes_count: 0, liked: false })),
      commentsApi.forPost(props.id).catch(() => []),
    ])
    post.value = p
    likes.value = l.likes_count
    liked.value = !!l.liked
    comments.value = c
  } catch (e: any) {
    error.value = e?.response?.data?.error ?? 'Post not found'
  } finally {
    loading.value = false
    commentsLoading.value = false
  }
}

onMounted(() => { load(); subscribeLive(props.id) })
watch(() => props.id, (newId, oldId) => {
  if (oldId) live.unsubscribePost(oldId)
  load()
  subscribeLive(newId)
})
onBeforeUnmount(() => { live.unsubscribePost(props.id) })

function subscribeLive(postId: number) {
  // Connect the WS lazily if the user happens to be authed; anonymous viewers
  // will see the snapshot only.
  if (auth.isAuthed) live.connectSocket()
  live.subscribePost(postId)
}

// Merge server-pushed live comments into the local list, dedup by id. The
// initial fetch + this watcher together guarantee we never miss one and
// never show duplicates if the REST roundtrip lands first.
watch(
  () => live.liveCommentsByPost.get(props.id)?.length ?? 0,
  () => {
    const incoming = live.liveCommentsByPost.get(props.id) ?? []
    const seen = new Set(comments.value.map(c => c.id))
    for (const c of incoming) {
      if (!seen.has(c.id)) {
        comments.value.push(c)
        seen.add(c.id)
      }
    }
  }
)

function formatDate(s: string) {
  return s ? new Date(s.replace(' ', 'T') + 'Z').toLocaleString() : ''
}
function isoDate(s: string) {
  const d = new Date(s.replace(' ', 'T') + 'Z')
  return isNaN(d.getTime()) ? '' : d.toISOString()
}

// One toggle rather than a Like button next to an Unlike button. The pair
// was not just noisy: neither knew the current state, so pressing Like
// twice incremented the counter locally while the server — which stores one
// like per (user, post) — had already ignored the second call.
async function toggleLike() {
  if (!auth.isAuthed || likeBusy.value) return
  const wasLiked = liked.value
  likeBusy.value = true
  // Optimistic: the round-trip is short but the button must not feel dead.
  liked.value = !wasLiked
  likes.value = Math.max(0, likes.value + (wasLiked ? -1 : 1))
  try {
    if (wasLiked) await postsApi.unlike(props.id)
    else          await postsApi.like(props.id)
  } catch (e: any) {
    liked.value = wasLiked
    likes.value = Math.max(0, likes.value + (wasLiked ? 1 : -1))
    toasts.push(e?.response?.data?.error ?? 'Could not update like', 'error')
  } finally {
    likeBusy.value = false
  }
}

async function submitComment() {
  if (!newComment.value.trim() || posting.value) return
  posting.value = true
  try {
    await commentsApi.create(props.id, newComment.value.trim())
    newComment.value = ''
    comments.value = await commentsApi.forPost(props.id)
  } catch (e: any) {
    toasts.push(e?.response?.data?.error ?? 'Could not post comment', 'error')
  } finally {
    posting.value = false
  }
}

async function deletePost() {
  if (!confirm('Delete this post?')) return
  try {
    await postsApi.remove(props.id)
    toasts.push('Post deleted', 'ok')
    router.push('/')
  } catch (e: any) {
    toasts.push(e?.response?.data?.error ?? 'Could not delete', 'error')
  }
}
</script>

<template>
  <!-- Skeleton rather than a "Loading…" line: the article is tall, so
       swapping a one-line placeholder for it moved everything below by
       most of a screen. -->
  <article v-if="loading" class="card post-skeleton" aria-hidden="true">
    <div class="row tight">
      <span class="avatar skeleton"></span>
      <div style="flex: 1;">
        <div class="skeleton line short"></div>
        <div class="skeleton line" style="width: 25%;"></div>
      </div>
    </div>
    <div class="skeleton line" style="height: 1.8em; width: 85%;"></div>
    <div class="skeleton line" style="height: 1.8em; width: 55%;"></div>
    <div class="skeleton line"></div>
    <div class="skeleton line"></div>
    <div class="skeleton line medium"></div>
  </article>
  <p v-if="loading" class="visually-hidden" role="status">Loading post…</p>

  <div v-else-if="error" class="empty-state" role="alert">
    <span class="emoji" aria-hidden="true">🔍</span>
    <p class="error">{{ error }}</p>
    <router-link to="/" class="btn ghost">Back to the feed</router-link>
  </div>

  <template v-else-if="post">
    <article class="card">
      <header class="row tight post-head">
        <span
          class="avatar"
          :style="post.author?.profile_image ? `background-image: url(${post.author.profile_image})` : ''"
          aria-hidden="true"
        ></span>
        <div class="post-head-meta">
          <router-link
            v-if="post.author"
            :to="{ name: 'profile', params: { id: post.author.id } }"
            class="post-head-author meta-link"
          >{{ post.author.username }}</router-link>
          <time v-if="isoDate(post.created_at)" :datetime="isoDate(post.created_at)" class="muted">
            {{ formatDate(post.created_at) }}
          </time>
        </div>
        <span class="spacer"></span>
        <button v-if="isOwner" class="ghost sm danger-text" @click="deletePost">Delete</button>
      </header>

      <h1 class="post-title">{{ post.title }}</h1>

      <!-- content_html is server-rendered by cmark-gfm in SAFE mode; raw
           HTML in the source is escaped before it ever reaches the client.
           sanitizePostHtml is a client-side second wall (defense-in-depth)
           that additionally wraps tables in a scroll container.
           Fall back to plain-text content for legacy rows where the column
           hasn't been backfilled. -->
      <div v-if="bodyHtml" class="post-body" v-html="bodyHtml"></div>
      <p v-else class="post-content">{{ post.content }}</p>

      <div class="row tight post-actions">
        <button
          v-if="auth.isAuthed"
          class="ghost"
          :class="{ liked }"
          :disabled="likeBusy"
          :aria-pressed="liked"
          @click="toggleLike"
        >
          <span aria-hidden="true">{{ liked ? '♥' : '♡' }}</span>
          {{ liked ? 'Liked' : 'Like' }}
        </button>
        <span class="muted">{{ likes }} like{{ likes === 1 ? '' : 's' }}</span>
      </div>
    </article>

    <section class="comments">
      <h2 class="comments-heading">Comments ({{ comments.length }})</h2>

      <form v-if="auth.isAuthed" @submit.prevent="submitComment" class="card">
        <label for="new-comment" class="visually-hidden">Write a comment</label>
        <textarea
          id="new-comment"
          v-model="newComment"
          placeholder="Write a comment…"
          rows="3"
          maxlength="2000"
        ></textarea>
        <div class="row tight" style="margin-top: var(--sp-3);">
          <button :disabled="!newComment.trim() || posting">
            {{ posting ? 'Posting…' : 'Post comment' }}
          </button>
        </div>
      </form>
      <p v-else class="muted">
        <router-link to="/login">Log in</router-link> to comment.
      </p>

      <div v-if="!commentsLoading && !comments.length" class="empty-state">
        <span class="emoji" aria-hidden="true">💬</span>
        <p>No comments yet. Be the first to say something.</p>
      </div>

      <article v-for="c in comments" :key="c.id" class="card comment">
        <header class="row tight comment-head">
          <strong v-if="c.author">{{ c.author.username }}</strong>
          <time v-if="isoDate(c.created_at)" :datetime="isoDate(c.created_at)" class="muted">
            {{ formatDate(c.created_at) }}
          </time>
        </header>
        <p class="post-content" style="margin: 0;">{{ c.content }}</p>
      </article>
    </section>
  </template>
</template>

<style scoped>
.post-skeleton { display: flex; flex-direction: column; gap: var(--sp-3); }

.post-head { margin-bottom: var(--sp-4); }
.post-head-meta { display: flex; flex-direction: column; line-height: 1.35; min-width: 0; }
.post-head-author { font-weight: 600; }
.post-head-meta time { font-size: 0.78rem; }

/* A red-filled Delete was the loudest thing on the page — on a phone it sat
   directly beside the author's name and read as the primary action. Quiet
   by default, unmistakably destructive on hover. */
.danger-text { color: var(--danger); }
.danger-text:hover { background: var(--danger-soft); color: var(--danger); }

/* h1 at --step-4 spent nine lines on a long title before the article even
   started. A post title is the page's heading, but it is read on a 390 px
   screen; one step down keeps the hierarchy and gives back half a screen. */
.post-title {
  font-size: var(--step-3);
  margin: 0 0 var(--sp-4);
  overflow-wrap: anywhere;
}

.post-actions { margin-top: var(--sp-5); }
.post-actions .liked { color: var(--danger); border-color: var(--danger); }

.comments { margin-top: var(--sp-6); }
.comments-heading { font-size: var(--step-1); margin: 0 0 var(--sp-4); }
.comment { margin-top: var(--sp-3); }
.comment-head { margin-bottom: var(--sp-2); }
.comment-head time { font-size: 0.78rem; }
</style>
