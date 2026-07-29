<script setup lang="ts">
import { ref, computed, watch, onMounted } from 'vue'
import { useRoute, useRouter } from 'vue-router'
import { postsApi, type SearchHit } from '@/api/posts'
import { sanitizeHtml } from '@/lib/sanitize'

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
    lastQuery.value = ''
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
function isoDate(s: string) {
  const d = new Date(s.replace(' ', 'T') + 'Z')
  return isNaN(d.getTime()) ? '' : d.toISOString()
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
  <h1 class="page-title">Search</h1>

  <form @submit.prevent="submit" class="card" role="search">
    <label for="search-q" class="visually-hidden">Search posts</label>
    <!-- No autofocus. On a phone it opens the keyboard the instant the route
         renders, covering half the screen before the reader has decided they
         want to type — and it steals focus from the skip link. -->
    <input
      id="search-q"
      v-model="input"
      type="search"
      placeholder="Search title and content…"
      enterkeyhint="search"
    />
    <div class="row tight" style="margin-top: var(--sp-3);">
      <button>Search</button>
      <span v-if="lastQuery && !loading" class="muted" role="status">
        {{ total }} result{{ total === 1 ? '' : 's' }} for “{{ lastQuery }}”
      </span>
    </div>
  </form>

  <template v-if="loading">
    <p class="visually-hidden" role="status">Searching…</p>
    <article v-for="n in 3" :key="n" class="card hit-skeleton" aria-hidden="true">
      <div class="row tight">
        <span class="avatar sm skeleton"></span>
        <div style="flex: 1;"><div class="skeleton line short"></div></div>
      </div>
      <div class="skeleton line" style="height: 1.3em; width: 60%;"></div>
      <div class="skeleton line medium"></div>
    </article>
  </template>

  <div v-else-if="error" class="empty-state" role="alert">
    <span class="emoji" aria-hidden="true">⚠️</span>
    <p class="error">{{ error }}</p>
  </div>

  <div v-else-if="q && !hits.length" class="empty-state">
    <span class="emoji" aria-hidden="true">🔍</span>
    <p>Nothing matched “{{ q }}”. Try fewer or more general words.</p>
  </div>

  <div v-else-if="!q" class="empty-state">
    <span class="emoji" aria-hidden="true">✨</span>
    <p>Type something above to search every post.</p>
  </div>

  <article v-for="h in hits" :key="h.id" class="card hit">
    <header class="row tight hit-head">
      <span
        class="avatar sm"
        :style="h.author?.profile_image ? `background-image: url(${h.author.profile_image})` : ''"
        aria-hidden="true"
      ></span>
      <div class="hit-meta">
        <router-link
          v-if="h.author"
          :to="{ name: 'profile', params: { id: h.author.id } }"
          class="hit-author meta-link"
        >{{ h.author.username }}</router-link>
        <span class="muted">
          <time v-if="isoDate(h.created_at)" :datetime="isoDate(h.created_at)">
            {{ formatDate(h.created_at) }}
          </time>
          · rank {{ h.rank.toFixed(3) }}
        </span>
      </div>
    </header>

    <h2 class="hit-title">
      <router-link :to="{ name: 'post', params: { id: h.id } }">{{ h.title }}</router-link>
    </h2>

    <!-- Snippet is server-built HTML containing only <mark> tags around hit
         terms; the rest is escaped by Postgres' ts_headline. sanitizeHtml is
         a client-side second wall (defense-in-depth). -->
    <p class="post-content snippet" v-html="sanitizeHtml(h.snippet)"></p>

    <router-link :to="{ name: 'post', params: { id: h.id } }" class="link-action">
      Open <span aria-hidden="true">→</span>
    </router-link>
  </article>
</template>

<style scoped>
.hit, .hit-skeleton { display: flex; flex-direction: column; gap: var(--sp-3); }
.hit-head { margin: 0; }
.hit-meta { display: flex; flex-direction: column; line-height: 1.35; min-width: 0; }
.hit-author { font-weight: 600; }
.hit-meta .muted { font-size: 0.78rem; }

/* A result is one row in a list, not a page heading — the global h2 scale
   runs to 2.15rem and made every hit shout. */
.hit-title { margin: 0; font-size: var(--step-1); line-height: 1.3; overflow-wrap: anywhere; }
.hit-title a { color: var(--text); }
.hit-title a:hover { color: var(--accent); text-decoration: none; }

.snippet { margin: 0; color: var(--text-dim); }
.snippet :deep(mark) {
  background: var(--accent-soft);
  color: var(--text);
  padding: 0 2px;
  border-radius: 2px;
}
</style>
