<script setup lang="ts">
import { useReadingPreferences } from '@/composables/useReadingPreferences'
import Avatar from '@/components/Avatar.vue'
import { formatDateTime, isoDate as toIsoDate, parseApiDate } from '@/lib/date'
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
import ReportButton from '@/components/ReportButton.vue'
import { socialApi } from '@/api/social'
import { usePageMeta } from '@/composables/usePageMeta'

const { size: readerSize, serif: readerSerif, measure: readerMeasure, reset: resetReading } = useReadingPreferences()
const focusMode = ref(false)
const commentsError = ref(false)
function printArticle() { window.print() }
function backToTop() {
  window.scrollTo({ top: 0, behavior: window.matchMedia('(prefers-reduced-motion: reduce)').matches ? 'instant' : 'smooth' })
}

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
const { setPageMeta } = usePageMeta()

const isOwner = computed(() => auth.isAuthed && post.value?.author?.id === auth.user!.id)

const bodyHtml = computed(() =>
  post.value?.content_html ? sanitizePostHtml(post.value.content_html) : '')

function descriptionFrom(content: string): string {
  return content
    .replace(/```[\s\S]*?```/g, ' ')
    .replace(/[`#>*_~()[\]!-]/g, ' ')
    .replace(/\s+/g, ' ')
    .trim()
    .slice(0, 160)
}

watch(post, (value) => {
  if (!value) return
  setPageMeta({
    title: value.title,
    description: descriptionFrom(value.content),
  })
})

// The rendered article element, handed to the table of contents so it can
// find the headings — they only exist after v-html has run.
const bodyEl = ref<HTMLElement | null>(null)

// Syntax highlighting runs after the body is in the DOM and the module is
// loaded lazily, so a reader who only ever opens prose posts never pays for
// the highlighter. Both are why this is a watcher rather than part of the
// render: highlight.js rewrites the <code> innerHTML in place.
watch([bodyHtml, bodyEl], async ([html, root]) => {
  if (!html || !root) return
  await nextTick()
  if (bodyEl.value !== root) return
  try {
    const { highlightWithin } = await import('@/lib/highlight')
    if (bodyEl.value !== root) return
    highlightWithin(root)
  } catch {
    // A chunk that fails to load leaves plain monospace code. That is a
    // worse-looking article, not a broken one.
  }
  if (bodyEl.value !== root) return
  addCopyButtons(root)
  updateProgress()
}, { immediate: true })

// One-click copy on every code block. Injected as real DOM nodes after the
// sanitized HTML is mounted (never through v-html), so the sanitizer's
// output is not widened to allow <button>.
function addCopyButtons(root: HTMLElement | null) {
  if (!root) return
  for (const pre of Array.from(root.querySelectorAll('pre'))) {
    if (pre.querySelector(':scope > .code-copy')) continue
    const btn = document.createElement('button')
    btn.type = 'button'
    btn.className = 'code-copy'
    btn.textContent = 'Copy'
    btn.setAttribute('aria-label', 'Copy code to clipboard')
    btn.addEventListener('click', async () => {
      const code = pre.querySelector('code')?.textContent ?? pre.textContent ?? ''
      try {
        await navigator.clipboard.writeText(code.replace(/\n$/, ''))
        btn.textContent = 'Copied ✓'
        btn.classList.add('done')
      } catch {
        btn.textContent = 'Press Ctrl+C'
      }
      setTimeout(() => { btn.textContent = 'Copy'; btn.classList.remove('done') }, 1800)
    })
    pre.appendChild(btn)
  }
}

// --- Reading progress ---------------------------------------------------
const progress = ref(0)
let progressRaf = 0
function updateProgress() {
  cancelAnimationFrame(progressRaf)
  progressRaf = requestAnimationFrame(() => {
    const el = bodyEl.value
    if (!el) { progress.value = 0; return }
    const rect = el.getBoundingClientRect()
    const total = rect.height - window.innerHeight * 0.6
    if (total <= 0) { progress.value = rect.top < window.innerHeight ? 100 : 0; return }
    const done = Math.min(Math.max(-rect.top + window.innerHeight * 0.2, 0), total)
    progress.value = Math.round((done / total) * 100)
  })
}
onMounted(() => {
  window.addEventListener('scroll', updateProgress, { passive: true })
  window.addEventListener('resize', updateProgress, { passive: true })
})
onBeforeUnmount(() => {
  window.removeEventListener('scroll', updateProgress)
  window.removeEventListener('resize', updateProgress)
  cancelAnimationFrame(progressRaf)
})

// --- Share --------------------------------------------------------------
// The /preview/posts/{id} page carries server-rendered Open Graph tags, so a
// link pasted into a chat app unfurls with the real title and excerpt, then
// offers readers a link into the interactive discussion.
const shareUrl = computed(() => `${window.location.origin}/preview/posts/${props.id}`)
async function sharePost() {
  if (!post.value) return
  const data = { title: post.value.title, text: post.value.excerpt ?? post.value.title, url: shareUrl.value }
  if (typeof navigator.share === 'function') {
    try { await navigator.share(data); return } catch (e: any) {
      if (e?.name === 'AbortError') return
    }
  }
  try {
    await navigator.clipboard.writeText(shareUrl.value)
    toasts.push('Link copied to clipboard', 'ok')
  } catch {
    toasts.push(shareUrl.value, 'ok')
  }
}

const wasEdited = computed(() => {
  const c = parseApiDate(post.value?.created_at)
  const u = parseApiDate(post.value?.updated_at)
  return !!c && !!u && u.getTime() - c.getTime() > 60_000
})

let loadSequence = 0
let disposed = false
async function load() {
  const sequence = ++loadSequence
  const postId = props.id
  loading.value = true
  commentsLoading.value = true
  error.value = ''
  commentsError.value = false
  try {
    const [p, l, c] = await Promise.all([
      postsApi.get(postId),
      postsApi.likesCount(postId).catch(() => ({ likes_count: 0, liked: false })),
      commentsApi.forPost(postId).catch(() => null),
    ])
    if (disposed || sequence !== loadSequence) return
    post.value = p
    likes.value = l.likes_count
    liked.value = !!l.liked
    bookmarked.value = !!p.bookmarked
    comments.value = c ?? []
    commentsError.value = c === null
  } catch (e: any) {
    if (disposed || sequence !== loadSequence) return
    error.value = e?.response?.data?.error ?? 'Post not found'
  } finally {
    if (!disposed && sequence === loadSequence) {
      loading.value = false
      commentsLoading.value = false
    }
  }
}

onMounted(() => { load(); subscribeLive(props.id) })
watch(() => props.id, (newId, oldId) => {
  focusMode.value = false
  replyingTo.value = null
  newComment.value = ''
  removedIds.clear()
  if (oldId) live.unsubscribePost(oldId)
  load()
  subscribeLive(newId)
})
onBeforeUnmount(() => { disposed = true; ++loadSequence; live.unsubscribePost(props.id) })

function subscribeLive(postId: number) {
  // Connect the WS lazily if the user happens to be authed; anonymous viewers
  // will see the snapshot only.
  if (auth.isAuthed) live.connectSocket()
  live.subscribePost(postId)
}

// Ids deleted from this view. The watcher below re-merges any comment it has
// seen over the socket that the list lacks, which would otherwise bring a
// deleted comment back the next time someone else posts.
const removedIds = new Set<number>()

// Merge server-pushed live comments into the local list, dedup by id. The
// initial fetch + this watcher together guarantee we never miss one and
// never show duplicates if the REST roundtrip lands first.
watch(
  () => live.liveCommentsByPost.get(props.id)?.length ?? 0,
  () => {
    const incoming = live.liveCommentsByPost.get(props.id) ?? []
    const seen = new Set(comments.value.map(c => c.id))
    for (const c of incoming) {
      if (!seen.has(c.id) && !removedIds.has(c.id)) {
        comments.value.push(c)
        seen.add(c.id)
      }
    }
  }
)

function formatDate(s: string) { return formatDateTime(s) }
function isoDate(s: string) { return toIsoDate(s) }

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

async function editComment(payload: { id: number; content: string }) {
  if (posting.value) return
  posting.value = true
  try {
    await commentsApi.update(payload.id, payload.content)
    comments.value = await commentsApi.forPost(props.id)
    toasts.push('Comment updated', 'ok')
  } catch (e: any) {
    toasts.push(e?.response?.data?.error ?? 'Could not update comment', 'error')
  } finally {
    posting.value = false
  }
}

async function removeComment(id: number) {
  if (posting.value) return
  posting.value = true
  try {
    const res = await commentsApi.remove(id)
    if (!res.tombstoned) removedIds.add(id)
    comments.value = await commentsApi.forPost(props.id)
    toasts.push(res.tombstoned ? 'Comment deleted — its replies were kept' : 'Comment deleted', 'ok')
  } catch (e: any) {
    toasts.push(e?.response?.data?.error ?? 'Could not delete comment', 'error')
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
  <div v-if="post && !loading && !error" class="read-progress" aria-hidden="true">
    <span :style="{ transform: `scaleX(${progress / 100})` }"></span>
  </div>
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
    <article class="card reader-article" :class="{ 'reader-focus': focusMode }">
      <header class="row tight post-head">
        <Avatar :image-url="post.author?.profile_image" :username="post.author?.username" />
        <div class="post-head-meta">
          <router-link
            v-if="post.author"
            :to="{ name: 'profile', params: { id: post.author.id } }"
            class="post-head-author meta-link"
          >{{ post.author.username }}</router-link>
          <time v-if="isoDate(post.created_at)" :datetime="isoDate(post.created_at)" class="muted">
            {{ formatDate(post.created_at) }}
          </time>
          <span v-if="wasEdited" class="faint edited-mark" :title="formatDate(post.updated_at)">
            edited {{ formatDate(post.updated_at) }}
          </span>
        </div>
        <span class="spacer"></span>
        <router-link v-if="isOwner" :to="{ name: 'edit-post', params: { id: post.id } }"
                     class="btn ghost sm">✏️ Edit</router-link>
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
      <div class="reading-tools">
        <details class="reading-options">
          <summary>{{ $t('reader.options') }}</summary>
          <div class="reading-settings">
            <div class="reader-field"><label for="reader-size">{{ $t('reader.text_size') }}</label>
              <select id="reader-size" v-model="readerSize">
                <option :value="0.95">{{ $t('reader.small') }}</option>
                <option :value="1">{{ $t('reader.standard') }}</option>
                <option :value="1.15">{{ $t('reader.large') }}</option>
                <option :value="1.3">{{ $t('reader.larger') }}</option>
              </select>
      </div>
            <div class="reader-field"><label for="reader-measure">{{ $t('reader.measure') }}</label>
              <select id="reader-measure" v-model="readerMeasure">
                <option value="full">{{ $t('reader.full') }}</option>
                <option value="comfortable">{{ $t('reader.comfortable') }}</option>
              </select>
      </div>
            <label class="reader-serif"><input v-model="readerSerif" type="checkbox"> {{ $t('reader.serif') }}</label>
            <button type="button" class="ghost sm" @click="resetReading">{{ $t('reader.reset') }}</button>
          </div>
        </details>
        <div class="reader-actions">
          <button type="button" class="ghost sm" :aria-pressed="focusMode" @click="focusMode = !focusMode">{{ $t(focusMode ? 'reader.exit_focus' : 'reader.focus') }}</button>
          <button type="button" class="ghost sm" @click="printArticle">{{ $t('reader.print') }}</button>
        </div>
      </div>

      <TableOfContents :body="bodyEl" :revision="bodyHtml" />

      <div v-if="bodyHtml" ref="bodyEl" class="post-body reader-body" :class="{ 'reader-serif-body': readerSerif }"
        :style="{ fontSize: `${readerSize}em`, maxWidth: readerMeasure === 'comfortable' ? '58ch' : undefined }" v-html="bodyHtml"></div>
      <p v-else class="post-content">{{ post.content }}</p>

      <div v-if="!focusMode" class="row tight post-actions">
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
        <button v-if="!post.is_draft" type="button" class="ghost" @click="sharePost">
          <span aria-hidden="true">🔗</span> Share
        </button>
        <span class="spacer"></span>
        <ReportButton v-if="auth.isAuthed && !isOwner" target-type="post" :target-id="post.id" />
      </div>
    </article>

    <div class="reader-return"><button type="button" class="ghost sm" @click="backToTop">↑ {{ $t('reader.top') }}</button></div>
    <section v-if="!focusMode" class="comments">
      <h2 class="comments-heading">Comments ({{ comments.filter(c => !c.deleted).length }})</h2>

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

      <p v-if="commentsError" class="error" role="alert">
        {{ $t('reader.comments_error') }}
        <button type="button" class="ghost sm" @click="load">{{ $t('reader.retry_comments') }}</button>
      </p>
      <div v-if="!commentsError && !commentsLoading && !comments.length" class="empty-state">
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
        @edit="editComment"
        @remove="removeComment"
      />
    </section>

    <!-- Last, deliberately: this is the "what next" once there is nothing
         left to read or reply to. -->
    <RelatedPosts v-if="!focusMode" :post-id="id" />
  </template>
</template>

<style scoped>
.reading-tools { margin-block: var(--sp-4); padding-block: var(--sp-3); border-block: 1px solid var(--border-soft); }
.reading-options summary { cursor: pointer; color: var(--text-dim); font-size: var(--step--1); padding-block: var(--sp-2); }
.reading-settings { display: flex; align-items: end; flex-wrap: wrap; gap: var(--sp-3); padding-block: var(--sp-3); }
.reader-field { min-width: 0; }
.reading-settings label { min-width: 0; font-size: var(--step--1); }
.reading-settings select { width: 100%; display: block; margin-top: var(--sp-1); }
.reading-settings .reader-serif { display: flex; align-items: center; gap: var(--sp-2); min-height: 2.75rem; }
.reader-actions { display: flex; flex-wrap: wrap; gap: var(--sp-2); }
.reader-body { margin-inline: auto; }
.reader-serif-body { font-family: Georgia, 'Times New Roman', serif; line-height: 1.85; }
.reader-focus { border-color: var(--accent); }
.reader-return { text-align: right; }
@media (max-width: 420px) { .reading-settings > label, .reading-settings > .reader-field { flex: 1 1 100%; } }
@media print {
  .reading-tools, .reader-return, .post-actions, .comments, :deep(.related), :deep(.toc), .read-progress, .post-head .btn, .post-head button { display: none !important; }
  .reader-article { border: 0; padding: 0; }
  .reader-body { font-size: 11pt !important; max-width: none !important; }
  .reader-article :deep(pre), .reader-article :deep(blockquote) { break-inside: avoid; }
  .reader-article :deep(.code-copy) { display: none !important; }
}

.read-progress {
  position: fixed; left: 0; right: 0; top: 0; height: 3px; z-index: 40;
  pointer-events: none;
}
.read-progress > span {
  display: block; height: 100%; background: var(--accent);
  transform-origin: 0 50%; transition: transform 80ms linear;
}
@media (prefers-reduced-motion: reduce) { .read-progress > span { transition: none; } }
.edited-mark { font-size: 0.75rem; }
.post-body :deep(pre) { position: relative; }
.post-body :deep(.code-copy) {
  position: absolute; top: var(--sp-2); right: var(--sp-2);
  padding: 0.15rem 0.55rem; font-size: 0.72rem; font-family: var(--font-sans, inherit);
  border-radius: var(--radius-sm); border: 1px solid var(--border);
  background: var(--bg-elev); color: var(--text-dim); box-shadow: none;
  opacity: 0; transition: opacity var(--dur, 150ms);
}
.post-body :deep(pre:hover .code-copy),
.post-body :deep(pre:focus-within .code-copy),
.post-body :deep(.code-copy.done) { opacity: 1; }
@media (hover: none) { .post-body :deep(.code-copy) { opacity: 1; } }
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
