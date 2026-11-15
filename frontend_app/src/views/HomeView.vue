<script setup lang="ts">
import { computed, onBeforeUnmount, onMounted, ref, watch } from 'vue'
import { postsApi, type Post, type TagSummary } from '@/api/posts'
import { socialApi } from '@/api/social'
import { useAuthStore } from '@/stores/auth'
import PostCard from '@/components/PostCard.vue'
import PostCardSkeleton from '@/components/PostCardSkeleton.vue'

const PAGE_SIZE = 20

const auth = useAuthStore()
const feedMode = ref<'latest' | 'following'>('latest')

const posts      = ref<Post[]>([])
const cursor     = ref<number | null>(null)
const hasMore    = ref(true)
const loading    = ref(false)
const error      = ref('')
const sentinel   = ref<HTMLElement | null>(null)
const topTags    = ref<TagSummary[]>([])

const activeTab = computed<'latest' | 'following'>(() =>
  auth.isAuthed ? feedMode.value : 'latest',
)

let observer: IntersectionObserver | null = null
let loadSeq = 0

async function loadMore(reset = false) {
  if (reset) {
    posts.value = []
    cursor.value = null
    hasMore.value = true
    error.value = ''
  }
  if (!reset && (loading.value || !hasMore.value)) return
  const seq = ++loadSeq
  const mode = activeTab.value
  loading.value = true
  try {
    const page = mode === 'following'
      ? await socialApi.followingFeedPage({
          before: cursor.value ?? undefined,
          limit:  PAGE_SIZE,
        })
      : await postsApi.list({
          before: cursor.value ?? undefined,
          limit:  PAGE_SIZE,
        })
    if (seq !== loadSeq) return
    const seen = new Set(posts.value.map(p => p.id))
    for (const p of page.posts) if (!seen.has(p.id)) posts.value.push(p)

    if (page.next_cursor != null) {
      cursor.value = page.next_cursor
    } else {
      hasMore.value = false
    }
  } catch (e: any) {
    if (seq !== loadSeq) return
    error.value = e?.response?.data?.error ?? 'Failed to load feed'
    hasMore.value = false
  } finally {
    if (seq === loadSeq) loading.value = false
  }
}

function selectTab(mode: 'latest' | 'following') {
  if (feedMode.value === mode) return
  feedMode.value = mode
  loadMore(true)
}

watch(() => auth.isAuthed, (authed) => {
  if (!authed && feedMode.value === 'following') {
    feedMode.value = 'latest'
    loadMore(true)
  }
})

onMounted(async () => {
  postsApi.listTags()
    .then(tags => { topTags.value = tags.slice(0, 8) })
    .catch(() => {})

  await loadMore(true)
  observer = new IntersectionObserver((entries) => {
    if (entries.some(e => e.isIntersecting)) loadMore()
  }, { rootMargin: '400px 0px' })
  if (sentinel.value) observer.observe(sentinel.value)
})

onBeforeUnmount(() => { observer?.disconnect() })
</script>

<template>
  <div class="feed-head">
    <h1>{{ $t('feed.heading') }}</h1>
    <div v-if="auth.isAuthed" class="tabs feed-tabs" role="tablist" :aria-label="$t('feed.mode_aria')">
      <button
        type="button" role="tab"
        :aria-selected="activeTab === 'latest'"
        :class="{ active: activeTab === 'latest' }"
        @click="selectTab('latest')"
      >{{ $t('feed.tab_latest') }}</button>
      <button
        type="button" role="tab"
        :aria-selected="activeTab === 'following'"
        :class="{ active: activeTab === 'following' }"
        @click="selectTab('following')"
      >{{ $t('feed.tab_following') }}</button>
    </div>
  </div>

  <nav v-if="topTags.length" class="tag-ribbon" :aria-label="$t('feed.popular_tags')">
    <router-link
      v-for="t in topTags"
      :key="t.slug"
      :to="{ name: 'tag', params: { slug: t.slug } }"
      class="tag-pill"
    >
      #{{ t.label }}
      <span class="faint tag-pill-count">{{ t.count }}</span>
    </router-link>
    <router-link :to="{ name: 'tags' }" class="tag-pill all">
      {{ $t('feed.all_tags') }} →
    </router-link>
  </nav>

  <div v-if="error" class="card" role="alert">
    <p class="error" style="margin: 0;">{{ error }}</p>
  </div>

  <PostCardSkeleton v-if="loading && !posts.length" :count="3" />

  <div v-else-if="!posts.length" class="empty-state">
    <span class="emoji" aria-hidden="true">{{ activeTab === 'following' ? '👥' : '📝' }}</span>
    <p>{{ activeTab === 'following' ? $t('feed.following_empty') : $t('feed.empty') }}</p>
    <button v-if="activeTab === 'following'" type="button" class="ghost" @click="selectTab('latest')">
      {{ $t('feed.browse_latest') }}
    </button>
  </div>

  <PostCard v-for="p in posts" :key="p.id" :post="p" clamp />

  <div ref="sentinel" aria-hidden="true"></div>

  <div aria-live="polite" :aria-busy="loading">
    <PostCardSkeleton v-if="loading && posts.length" :count="1" />
  </div>

  <p v-if="!loading && !hasMore && posts.length" class="muted"
     style="text-align: center; padding-block: var(--sp-4);">
    — {{ $t('feed.heading') }} —
  </p>
</template>

<style scoped>
.feed-head {
  display: flex;
  flex-wrap: wrap;
  align-items: center;
  justify-content: space-between;
  gap: var(--sp-3);
  margin-bottom: var(--sp-4);
}
.feed-head h1 { margin: 0; }
.feed-tabs { margin: 0; }

.tag-ribbon {
  display: flex;
  gap: var(--sp-2);
  overflow-x: auto;
  scrollbar-width: thin;
  padding-bottom: var(--sp-2);
  margin-bottom: var(--sp-4);
}
.tag-pill {
  display: inline-flex;
  align-items: center;
  gap: 0.35em;
  flex: 0 0 auto;
  padding: 0.3rem 0.7rem;
  border-radius: 999px;
  border: 1px solid var(--border);
  background: var(--bg-elev);
  color: var(--text-dim);
  font-size: var(--step--1);
  white-space: nowrap;
  text-decoration: none;
}
.tag-pill:hover {
  border-color: var(--accent);
  color: var(--accent);
  text-decoration: none;
}
.tag-pill.all { border-style: dashed; }
.tag-pill-count { font-size: 0.75em; font-variant-numeric: tabular-nums; }
</style>
