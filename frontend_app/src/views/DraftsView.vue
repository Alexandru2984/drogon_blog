<script setup lang="ts">
import { ref, onMounted } from 'vue'
import { postsApi, type Post } from '@/api/posts'
import { useToastStore } from '@/stores/toast'
import TagList from '@/components/TagList.vue'
import PostMeta from '@/components/PostMeta.vue'

const drafts  = ref<Post[]>([])
const loading = ref(true)
const error   = ref('')
const busyId  = ref<number | null>(null)

const toasts = useToastStore()

async function load() {
  loading.value = true
  try {
    drafts.value = await postsApi.myDrafts()
  } catch (e: any) {
    error.value = e?.response?.data?.error ?? 'Could not load your drafts'
  } finally {
    loading.value = false
  }
}
onMounted(load)

async function publish(p: Post) {
  busyId.value = p.id
  try {
    await postsApi.update(p.id, { draft: false })
    // Drop it locally rather than refetching: the list is short and the
    // round trip would make the row linger visibly after the action.
    drafts.value = drafts.value.filter(d => d.id !== p.id)
    toasts.push('Published', 'ok')
  } catch (e: any) {
    toasts.push(e?.response?.data?.error ?? 'Could not publish', 'error')
  } finally {
    busyId.value = null
  }
}

async function discard(p: Post) {
  if (!confirm(`Delete the draft “${p.title}”? This cannot be undone.`)) return
  busyId.value = p.id
  try {
    await postsApi.remove(p.id)
    drafts.value = drafts.value.filter(d => d.id !== p.id)
    toasts.push('Draft deleted', 'ok')
  } catch (e: any) {
    toasts.push(e?.response?.data?.error ?? 'Could not delete', 'error')
  } finally {
    busyId.value = null
  }
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
  <h1 class="page-title">Drafts</h1>

  <template v-if="loading">
    <p class="visually-hidden" role="status">Loading drafts…</p>
    <div v-for="n in 2" :key="n" class="card" aria-hidden="true">
      <div class="skeleton line" style="height: 1.4em; width: 55%;"></div>
      <div class="skeleton line medium"></div>
    </div>
  </template>

  <div v-else-if="error" class="empty-state" role="alert">
    <span class="emoji" aria-hidden="true">⚠️</span>
    <p class="error">{{ error }}</p>
  </div>

  <div v-else-if="!drafts.length" class="empty-state">
    <span class="emoji" aria-hidden="true">📄</span>
    <p>No drafts. Anything you save without publishing shows up here.</p>
    <router-link :to="{ name: 'create-post' }" class="btn">Start writing</router-link>
  </div>

  <article v-for="d in drafts" :key="d.id" class="card draft">
    <h2 class="draft-title">
      <!-- A draft is only readable by its author, so linking to the post
           route is safe and is where editing will happen. -->
      <router-link :to="{ name: 'post', params: { id: d.id } }">
        {{ d.title || 'Untitled draft' }}
      </router-link>
    </h2>

    <PostMeta :reading-minutes="d.reading_minutes" is-draft />

    <p v-if="d.excerpt" class="muted draft-excerpt">{{ d.excerpt }}</p>

    <TagList :tags="d.tags" small />

    <p class="faint draft-when">
      Last edited
      <time v-if="iso(d.updated_at)" :datetime="iso(d.updated_at)">{{ when(d.updated_at) }}</time>
    </p>

    <div class="row tight">
      <button :disabled="busyId === d.id" @click="publish(d)">
        {{ busyId === d.id ? 'Publishing…' : 'Publish' }}
      </button>
      <button class="ghost sm danger-text" :disabled="busyId === d.id" @click="discard(d)">
        Delete
      </button>
    </div>
  </article>
</template>

<style scoped>
.draft { display: flex; flex-direction: column; gap: var(--sp-3); }
.draft-title { margin: 0; font-size: var(--step-1); line-height: 1.3; overflow-wrap: anywhere; }
.draft-title a { color: var(--text); }
.draft-title a:hover { color: var(--accent); text-decoration: none; }
.draft-excerpt { margin: 0; }
.draft-when { margin: 0; font-size: var(--step--1); }

.danger-text { color: var(--danger); }
.danger-text:hover { background: var(--danger-soft); color: var(--danger); }
</style>
