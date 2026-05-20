<script setup lang="ts">
import { ref, computed, onMounted, watch } from 'vue'
import { useRouter } from 'vue-router'
import { postsApi, type Post } from '@/api/posts'
import { commentsApi, type Comment } from '@/api/comments'
import { useAuthStore } from '@/stores/auth'
import { useToastStore } from '@/stores/toast'

const props = defineProps<{ id: number }>()

const post = ref<Post | null>(null)
const likes = ref<number>(0)
const comments = ref<Comment[]>([])
const newComment = ref('')
const loading = ref(true)
const error = ref('')

const auth = useAuthStore()
const router = useRouter()
const toasts = useToastStore()

const isOwner = computed(() => auth.isAuthed && post.value?.author?.id === auth.user!.id)

async function load() {
  loading.value = true
  error.value = ''
  try {
    const [p, l, c] = await Promise.all([
      postsApi.get(props.id),
      postsApi.likesCount(props.id).catch(() => ({ likes_count: 0 })),
      commentsApi.forPost(props.id).catch(() => []),
    ])
    post.value = p
    likes.value = l.likes_count
    comments.value = c
  } catch (e: any) {
    error.value = e?.response?.data?.error ?? 'Post not found'
  } finally {
    loading.value = false
  }
}

onMounted(load)
watch(() => props.id, load)

function formatDate(s: string) {
  return s ? new Date(s.replace(' ', 'T') + 'Z').toLocaleString() : ''
}

async function like() {
  try { await postsApi.like(props.id); likes.value++ }
  catch (e: any) { toasts.push(e?.response?.data?.error ?? 'Could not like', 'error') }
}
async function unlike() {
  try { await postsApi.unlike(props.id); likes.value = Math.max(0, likes.value - 1) }
  catch (e: any) { toasts.push(e?.response?.data?.error ?? 'Could not unlike', 'error') }
}

async function submitComment() {
  if (!newComment.value.trim()) return
  try {
    await commentsApi.create(props.id, newComment.value.trim())
    newComment.value = ''
    comments.value = await commentsApi.forPost(props.id)
  } catch (e: any) {
    toasts.push(e?.response?.data?.error ?? 'Could not post comment', 'error')
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
  <p v-if="loading" class="muted">Loading…</p>
  <p v-else-if="error" class="error">{{ error }}</p>

  <template v-else-if="post">
    <article class="card">
      <header class="toolbar" style="margin-bottom: 0.75rem;">
        <span
          class="avatar"
          :style="post.author?.profile_image ? `background-image: url(${post.author.profile_image})` : ''"
        ></span>
        <div>
          <router-link
            v-if="post.author"
            :to="{ name: 'profile', params: { id: post.author.id } }"
            style="font-weight: 600;"
          >{{ post.author.username }}</router-link>
          <div class="muted" style="font-size: 0.8em;">{{ formatDate(post.created_at) }}</div>
        </div>
        <span class="spacer"></span>
        <template v-if="isOwner">
          <button class="danger" @click="deletePost">Delete</button>
        </template>
      </header>

      <h1>{{ post.title }}</h1>
      <!-- content_html is server-rendered by cmark-gfm in SAFE mode; raw
           HTML in the source is escaped before it ever reaches the client.
           Fall back to plain-text content for legacy rows where the column
           hasn't been backfilled. -->
      <div v-if="post.content_html" class="post-body" v-html="post.content_html"></div>
      <p v-else class="post-content">{{ post.content }}</p>

      <div class="toolbar" style="margin-top: 1rem;">
        <button v-if="auth.isAuthed" class="ghost" @click="like">♥ Like</button>
        <button v-if="auth.isAuthed" class="ghost" @click="unlike">Unlike</button>
        <span class="muted">{{ likes }} like{{ likes === 1 ? '' : 's' }}</span>
      </div>
    </article>

    <section style="margin-top: 1.5rem;">
      <h3>Comments ({{ comments.length }})</h3>

      <form v-if="auth.isAuthed" @submit.prevent="submitComment" class="card">
        <textarea v-model="newComment" placeholder="Write a comment…" rows="3"></textarea>
        <div class="toolbar" style="margin-top: 0.5rem;">
          <button :disabled="!newComment.trim()">Post comment</button>
        </div>
      </form>
      <p v-else class="muted">
        <router-link to="/login">Log in</router-link> to comment.
      </p>

      <article v-for="c in comments" :key="c.id" class="card">
        <header class="toolbar" style="margin-bottom: 0.4rem;">
          <strong v-if="c.author">{{ c.author.username }}</strong>
          <span class="muted" style="font-size: 0.8em;">{{ formatDate(c.created_at) }}</span>
        </header>
        <p class="post-content" style="margin: 0;">{{ c.content }}</p>
      </article>
    </section>
  </template>
</template>
