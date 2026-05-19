<script setup lang="ts">
import { ref, computed, watch, onMounted } from 'vue'
import { useRoute, useRouter } from 'vue-router'
import { postsApi, type SearchHit } from '@/api/posts'

const route  = useRoute()
const router = useRouter()

const input    = ref('')
const hits     = ref<SearchHit[]>([])
const total    = ref(0)
const loading  = ref(false)
const error    = ref('')
const lastQuery = ref('')

const q = computed(() => String(route.query.q ?? '').trim())

async function run(query: string) {
  if (!query) {
    hits.value = []
    total.value = 0
    return
  }
  loading.value = true
  error.value = ''
  try {
    const res = await postsApi.search(query)
    hits.value = res.posts
    total.value = res.count
    lastQuery.value = query
  } catch (e: any) {
    error.value = e?.response?.data?.error ?? 'Search failed'
  } finally {
    loading.value = false
  }
}

function submit() {
  const next = input.value.trim()
  router.push({ name: 'search', query: next ? { q: next } : {} })
}

function formatDate(s: string) {
  return s ? new Date(s.replace(' ', 'T') + 'Z').toLocaleString() : ''
}

onMounted(() => {
  input.value = q.value
  run(q.value)
})
watch(q, (v) => {
  input.value = v
  run(v)
})
</script>

<template>
  <h1>Search</h1>

  <form @submit.prevent="submit" class="card">
    <input
      v-model="input"
      type="search"
      placeholder="Search title and content…"
      autofocus
    />
    <div class="toolbar" style="margin-top: 0.75rem;">
      <button>Search</button>
      <span v-if="lastQuery && !loading" class="muted">
        {{ total }} result{{ total === 1 ? '' : 's' }} for “{{ lastQuery }}”
      </span>
    </div>
  </form>

  <p v-if="loading" class="muted">Searching…</p>
  <p v-else-if="error" class="error">{{ error }}</p>
  <p v-else-if="q && !hits.length" class="muted">No matches.</p>

  <article v-for="h in hits" :key="h.id" class="card">
    <header class="toolbar" style="margin-bottom: 0.5rem;">
      <span
        class="avatar"
        :style="h.author?.profile_image ? `background-image: url(${h.author.profile_image})` : ''"
      ></span>
      <div>
        <router-link
          v-if="h.author"
          :to="{ name: 'profile', params: { id: h.author.id } }"
          style="font-weight: 600;"
        >{{ h.author.username }}</router-link>
        <div class="muted" style="font-size: 0.8em;">
          {{ formatDate(h.created_at) }} · rank {{ h.rank.toFixed(3) }}
        </div>
      </div>
    </header>

    <router-link :to="{ name: 'post', params: { id: h.id } }" style="color: var(--text);">
      <h2 style="margin-bottom: 0.25rem;">{{ h.title }}</h2>
    </router-link>

    <!-- Snippet is server-built HTML containing only <mark> tags around hit
         terms; the rest is escaped by Postgres' ts_headline. -->
    <p class="post-content snippet" v-html="h.snippet"></p>

    <div class="toolbar muted" style="margin-top: 0.5rem;">
      <router-link :to="{ name: 'post', params: { id: h.id } }">Open →</router-link>
    </div>
  </article>
</template>

<style scoped>
.snippet :deep(mark) {
  background: rgba(124, 92, 255, 0.25);
  color: var(--text);
  padding: 0 2px;
  border-radius: 2px;
}
</style>
