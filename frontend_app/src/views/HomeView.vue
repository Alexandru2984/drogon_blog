<script setup lang="ts">
import { computed, onBeforeUnmount, onMounted, ref, watch } from 'vue'
import { useRoute, useRouter } from 'vue-router'
import { useI18n } from 'vue-i18n'
import { postsApi, type Post, type TagSummary } from '@/api/posts'
import { socialApi } from '@/api/social'
import { useAuthStore } from '@/stores/auth'
import PostCard from '@/components/PostCard.vue'
import PostCardSkeleton from '@/components/PostCardSkeleton.vue'

const PAGE_SIZE = 12
const auth = useAuthStore()
const route = useRoute()
const router = useRouter()
const { t } = useI18n()
type FeedMode = 'latest' | 'popular' | 'discussed' | 'following'
const activeTab = computed<FeedMode>(() => {
  const value = route.query.feed
  if (value === 'popular' || value === 'discussed') return value
  return value === 'following' && auth.isAuthed ? 'following' : 'latest'
})
const selectedTag = computed(() => typeof route.query.tag === 'string' ? route.query.tag.slice(0, 80) : '')
const period = computed<'all' | 'week' | 'month'>(() =>
  route.query.period === 'week' || route.query.period === 'month' ? route.query.period : 'all')
const minutes = computed(() => route.query.read === '5' || route.query.read === '10' ? Number(route.query.read) : 0)
const filtersOpen = ref(window.matchMedia('(min-width: 481px)').matches)
const filtered = computed(() => !!selectedTag.value || period.value !== 'all' || minutes.value > 0)
const posts = ref<Post[]>([])
const nextPage = ref<number | null>(0)
const loading = ref(false)
const error = ref('')
const sentinel = ref<HTMLElement | null>(null)
const tags = ref<TagSummary[]>([])
const tabs = computed(() => [
  { key: 'latest', label: t('feed.tab_latest') },
  { key: 'popular', label: t('feed.tab_popular') },
  { key: 'discussed', label: t('feed.tab_discussed') },
  ...(auth.isAuthed ? [{ key: 'following', label: t('feed.tab_following') }] : []),
])
let observer: IntersectionObserver | null = null
let loadSeq = 0
let controller: AbortController | null = null
let disposed = false

async function loadMore(reset = false) {
  if (!reset && (loading.value || nextPage.value === null)) return
  controller?.abort()
  controller = new AbortController()
  const seq = ++loadSeq
  if (reset) { posts.value = []; nextPage.value = 0 }
  loading.value = true
  error.value = ''
  try {
    const mode = activeTab.value
    const page = mode === 'following'
      ? await socialApi.followingFeedPage({ before: nextPage.value || undefined, limit: PAGE_SIZE })
      : await postsApi.discover({ sort: mode, period: period.value, tag: selectedTag.value || undefined,
          max_minutes: minutes.value, offset: nextPage.value ?? 0, limit: PAGE_SIZE }, controller.signal)
    if (seq !== loadSeq || disposed) return
    const seen = new Set(posts.value.map(p => p.id))
    for (const post of page.posts) {
      if (!seen.has(post.id)) { posts.value.push(post); seen.add(post.id) }
    }
    nextPage.value = ('next_offset' in page ? page.next_offset : page.next_cursor) ?? null
  } catch (e: any) {
    if (seq !== loadSeq || disposed || e?.code === 'ERR_CANCELED') return
    error.value = e?.response?.data?.error ?? t('feed.load_error')
  } finally {
    if (seq === loadSeq && !disposed) loading.value = false
  }
}

function setFilter(key: string, value: string) {
  router.replace({ query: { ...route.query, [key]: value || undefined } })
}
function clearFilters() {
  router.replace({ query: { ...route.query, tag: undefined, period: undefined, read: undefined } })
}
watch([activeTab, selectedTag, period, minutes, () => auth.user?.id], () => loadMore(true), { immediate: true })
onMounted(() => {
  postsApi.listTags().then(value => { if (!disposed) tags.value = value }).catch(() => {})
  observer = new IntersectionObserver(entries => {
    if (!error.value && entries.some(e => e.isIntersecting)) loadMore()
  }, { rootMargin: '200px 0px' })
  if (sentinel.value) observer.observe(sentinel.value)
})
onBeforeUnmount(() => { disposed = true; ++loadSeq; controller?.abort(); observer?.disconnect() })
</script>

<template>
  <section class="feed-welcome" aria-labelledby="feed-title">
    <div>
      <p class="feed-eyebrow">{{ $t('feed.eyebrow') }}</p>
      <h1 id="feed-title">{{ $t('feed.heading') }}</h1>
      <p class="feed-intro">{{ $t('feed.intro') }}</p>
    </div>
    <router-link :to="auth.isAuthed ? '/posts/new' : '/register'" class="btn">
      {{ $t(auth.isAuthed ? 'feed.start_writing' : 'feed.join') }} <span aria-hidden="true">↗</span>
    </router-link>
  </section>

  <div class="feed-controls card">
    <div class="feed-modes" role="group" :aria-label="$t('feed.mode_aria')">
      <button v-for="tab in tabs" :key="tab.key" type="button" class="ghost sm"
        :class="{ active: activeTab === tab.key }" :aria-pressed="activeTab === tab.key"
        @click="setFilter('feed', tab.key === 'latest' ? '' : tab.key)">{{ tab.label }}</button>
    </div>
    <details v-if="activeTab !== 'following'" class="filter-details" :open="filtersOpen" @toggle="filtersOpen = ($event.target as HTMLDetailsElement).open">
      <summary>{{ $t('feed.filters') }}<span v-if="filtered" class="filter-indicator" aria-hidden="true"> · {{ [!!selectedTag, period !== 'all', minutes > 0].filter(Boolean).length }}</span></summary>
      <div class="discovery-filters" role="group" :aria-label="$t('feed.filters')">
      <div class="filter-field"><label for="feed-topic">{{ $t('feed.topic') }}</label>
        <select id="feed-topic" :value="selectedTag" @change="setFilter('tag', ($event.target as HTMLSelectElement).value)">
          <option value="">{{ $t('feed.all_topics') }}</option>
          <option v-if="selectedTag && !tags.some(tag => tag.slug === selectedTag)" :value="selectedTag">#{{ selectedTag }}</option>
          <option v-for="tag in tags" :key="tag.slug" :value="tag.slug">#{{ tag.label }} ({{ tag.count }})</option>
        </select>
      </div>
      <div class="filter-field"><label for="feed-period">{{ $t('feed.period') }}</label>
        <select id="feed-period" :value="period" @change="setFilter('period', ($event.target as HTMLSelectElement).value)">
          <option value="all">{{ $t('feed.all_time') }}</option>
          <option value="week">{{ $t('feed.week') }}</option>
          <option value="month">{{ $t('feed.month') }}</option>
        </select>
      </div>
      <div class="filter-field"><label for="feed-reading">{{ $t('feed.reading_time') }}</label>
        <select id="feed-reading" :value="minutes" @change="setFilter('read', ($event.target as HTMLSelectElement).value)">
          <option value="0">{{ $t('feed.any_length') }}</option>
          <option value="5">{{ $t('feed.under_five') }}</option>
          <option value="10">{{ $t('feed.under_ten') }}</option>
        </select>
      </div>
    </div>
    </details>
    <div class="feed-utility">
      <button v-if="filtered && activeTab !== 'following'" type="button" class="ghost sm" @click="clearFilters">{{ $t('feed.reset_filters') }}</button>
      <router-link to="/tags">{{ $t('feed.all_tags') }} →</router-link>
      <a href="/feed.xml">{{ $t('feed.rss') }} ↗</a>
    </div>
  </div>

  <div v-if="error" class="card feed-error" role="alert">
    <p class="error">{{ error }}</p>
    <button type="button" class="ghost" @click="loadMore(!posts.length)">{{ $t('feed.retry') }}</button>
  </div>
  <PostCardSkeleton v-if="loading && !posts.length" :count="3" />
  <div v-else-if="!posts.length && !error" class="empty-state">
    <span class="emoji" aria-hidden="true">{{ activeTab === 'following' ? '👥' : '📝' }}</span>
    <p>{{ $t(activeTab === 'following' ? 'feed.following_empty' : filtered ? 'feed.no_matches' : 'feed.empty') }}</p>
    <button v-if="activeTab === 'following'" type="button" class="ghost" @click="setFilter('feed', '')">{{ $t('feed.browse_latest') }}</button>
    <button v-else-if="filtered" type="button" class="ghost" @click="clearFilters">{{ $t('feed.reset_filters') }}</button>
  </div>
  <PostCard v-for="post in posts" :key="post.id" :post="post" clamp />
  <div ref="sentinel" aria-hidden="true"></div>
  <div class="feed-pagination" aria-live="polite" :aria-busy="loading">
    <PostCardSkeleton v-if="loading && posts.length" :count="1" />
    <button v-if="!loading && !error && nextPage !== null && posts.length" type="button" class="ghost" @click="loadMore()">{{ $t('feed.load_more') }}</button>
    <p v-if="!loading && !error && nextPage === null && posts.length" class="muted">{{ $t('feed.end') }} <span aria-hidden="true">✦</span></p>
  </div>
</template>

<style scoped>
.feed-welcome { display: flex; flex-wrap: wrap; align-items: center; justify-content: space-between; gap: var(--sp-4); padding-block: var(--sp-4) var(--sp-6); }
.feed-welcome > div { flex: 1 1 20rem; min-width: 0; }
.feed-welcome h1 { margin: 0 0 var(--sp-2); }
.feed-eyebrow { color: var(--accent); font-size: 0.7rem; font-weight: 700; letter-spacing: 0.1em; margin: 0 0 var(--sp-3); }
.feed-intro { color: var(--text-dim); margin: 0; max-width: 35ch; }
.feed-welcome .btn { flex-shrink: 0; gap: var(--sp-3); }
.feed-controls { padding: var(--sp-3); border-top: 3px solid var(--accent); }
.feed-modes { display: flex; flex-wrap: wrap; gap: var(--sp-1); padding-bottom: var(--sp-3); }
.feed-modes .active { background: var(--accent-soft); color: var(--accent); border-color: var(--accent); }
.filter-details summary { cursor: pointer; font-size: var(--step--1); color: var(--text-dim); padding-block: var(--sp-2); }
.filter-indicator { color: var(--accent); font-weight: 700; }
.discovery-filters { display: grid; grid-template-columns: 1.2fr 1fr 1fr; gap: var(--sp-3); padding-top: var(--sp-3); border-top: 1px solid var(--border-soft); }
.filter-field { min-width: 0; }
.discovery-filters label { min-width: 0; font-size: var(--step--1); color: var(--text-dim); }
.discovery-filters select { display: block; width: 100%; min-width: 0; margin-top: var(--sp-1); font-size: var(--step--1); }
.feed-utility { display: flex; flex-wrap: wrap; align-items: center; gap: var(--sp-3); margin-top: var(--sp-3); font-size: var(--step--1); }
.feed-utility a:last-child { margin-left: auto; }
.feed-error p { margin: 0 0 var(--sp-3); }
.feed-pagination { text-align: center; padding-block: var(--sp-4); }
@media (max-width: 480px) { .discovery-filters { grid-template-columns: 1fr; gap: var(--sp-2); } }
</style>
