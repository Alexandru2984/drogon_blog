<script setup lang="ts">
import { ref, computed, onBeforeUnmount, onMounted, watch, nextTick } from 'vue'
import { useRouter } from 'vue-router'
import { postsApi, type Post } from '@/api/posts'
import { commentsApi, type Comment } from '@/api/comments'
import { useAuthStore } from '@/stores/auth'
import { useMessagesStore } from '@/stores/messages'
import { useToastStore } from '@/stores/toast'
import { sanitizePostHtml } from '@/lib/sanitize'
import TagList from '@/components/TagList.vue'
import PostMeta from '@/components/PostMeta.vue'
import CommentThread from '@/components/CommentThread.vue'
import TableOfContents from '@/components/TableOfContents.vue'
import RelatedPosts from '@/components/RelatedPosts.vue'
import { socialApi } from '@/api/social'

const props = defineProps<{ id: number }>()

const post = ref<Post | null>(null)
const likes = ref<number>(0)
const liked = ref(false)
const likeBusy = ref(false)
const comments = ref<Comment[]>([])
const commentsLoading = ref(true)
const newComment = ref('')
const posting = ref(false)
const publishing = ref(false)
const bookmarked = ref(false)
const bookmarkBusy = ref(false)
const replyingTo = ref<number | null>(null)
const loading = ref(true)
const error = ref('')

const auth     = useAuthStore()
const router   = useRouter()
const toasts   = useToastStore()
const live     = useMessagesStore()

const isOwner = computed(() => auth.isAuthed && post.value?.author?.id === auth.user!.id)

const bodyHtml = computed(() =>
  post.value?.content_html ? sanitizePostHtml(post.value.content_html) : '')

// The rendered article element, handed to the table of contents so it can
// find the headings — they only exist after v-html has run.
const bodyEl = ref<HTMLElement | null>(null)

// Syntax highlighting runs after the body is in the DOM and the module is
// loaded lazily, so a reader who only ever opens prose posts never pays for
// the highlighter. Both are why this is a watcher rather than part of the
// render: highlight.js rewrites the <code> innerHTML in place.
watch(bodyHtml, async (html) => {
  if (!html) return
  await nextTick()
  if (!bodyEl.value) return
  try {
    const { highlightWithin } = await import('@/lib/highlight')
    highlightWithin(bodyEl.value)
  } catch {
    // A chunk that fails to load leaves plain monospace code. That is a
    // worse-looking article, not a broken one.
  }
}, { immediate: true })

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
    bookmarked.value = !!p.bookmarked
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

async function toggleBookmark() {
  if (!auth.isAuthed || bookmarkBusy.value) return
  const was = bookmarked.value
  bookmarkBusy.value = true
  bookmarked.value = !was
  try {
    if (was) await socialApi.removeBookmark(props.id)
    else     await socialApi.addBookmark(props.id)
  } catch (e: any) {
    bookmarked.value = was
    toasts.push(e?.response?.data?.error ?? 'Could not update bookmark', 'error')
  } finally {
    bookmarkBusy.value = false
  }
}

async function submitReply(payload: { parentId: number; content: string }) {
  if (posting.value) return
  posting.value = true
  try {
    await commentsApi.create(props.id, payload.content, payload.parentId)
    replyingTo.value = null
    comments.value = await commentsApi.forPost(props.id)
  } catch (e: any) {
    toasts.push(e?.response?.data?.error ?? 'Could not post reply', 'error')
  } finally {
    posting.value = false
  }
}

async function publishDraft() {
  if (!post.value || publishing.value) return
  publishing.value = true
  try {
    await postsApi.update(props.id, { draft: false })
    // Re-read rather than patching locally: publishing stamps published_at
    // server-side and that is the value the view should show.
    await load()
    toasts.push('Published', 'ok')
  } catch (e: any) {
    toasts.push(e?.response?.data?.error ?? 'Could not publish', 'error')
  } finally {
    publishing.value = false
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

      <PostMeta
        :reading-minutes="post.reading_minutes"
        :view-count="post.view_count"
        :is-draft="post.is_draft"
        class="post-meta-line"
      />

      <div v-if="post.tags && post.tags.length" class="post-tags">
        <TagList :tags="post.tags" />
      </div>

      <!-- content_html is server-rendered by cmark-gfm in SAFE mode; raw
           HTML in the source is escaped before it ever reaches the client.
           sanitizePostHtml is a client-side second wall (defense-in-depth)
           that additionally wraps tables in a scroll container.
           Fall back to plain-text content for legacy rows where the column
           hasn't been backfilled. -->
      <TableOfContents :body="bodyEl" :revision="bodyHtml" />

      <div v-if="bodyHtml" ref="bodyEl" class="post-body" v-html="bodyHtml"></div>
      <p v-else class="post-content">{{ post.content }}</p>

      <div class="row tight post-actions">
        <!-- Publishing from the post itself, because that is where the
             author lands after saving a draft and re-reading it. -->
        <button v-if="isOwner && post.is_draft" :disabled="publishing" @click="publishDraft">
          {{ publishing ? 'Publishing…' : 'Publish' }}
        </button>
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
        <button
          v-if="auth.isAuthed"
          class="ghost"
          :class="{ saved: bookmarked }"
          :disabled="bookmarkBusy"
          :aria-pressed="bookmarked"
          @click="toggleBookmark"
        >
          <span aria-hidden="true">{{ bookmarked ? '🔖' : '📑' }}</span>
          {{ bookmarked ? 'Saved' : 'Save' }}
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

      <CommentThread
        :comments="comments"
        :can-reply="auth.isAuthed"
        :replying-to="replyingTo"
        :posting="posting"
        @reply="(id) => (replyingTo = id)"
        @submit="submitReply"
      />
    </section>

    <!-- Last, deliberately: this is the "what next" once there is nothing
         left to read or reply to. -->
    <RelatedPosts :post-id="id" />
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
  margin: 0 0 var(--sp-3);
  overflow-wrap: anywhere;
}
.post-meta-line { margin-bottom: var(--sp-3); }
.post-tags { margin-bottom: var(--sp-5); }

.post-actions { margin-top: var(--sp-5); }
.post-actions .liked { color: var(--danger); border-color: var(--danger); }
.post-actions .saved { color: var(--accent); border-color: var(--accent); }

.comments { margin-top: var(--sp-6); }
.comments-heading { font-size: var(--step-1); margin: 0 0 var(--sp-4); }
</style>
