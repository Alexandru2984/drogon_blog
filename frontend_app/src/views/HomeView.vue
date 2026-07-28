<script setup lang="ts">
import { onBeforeUnmount, onMounted, ref } from 'vue'
import { postsApi, type Post } from '@/api/posts'
import PostCard from '@/components/PostCard.vue'
import PostCardSkeleton from '@/components/PostCardSkeleton.vue'

const PAGE_SIZE = 20

const posts      = ref<Post[]>([])
const cursor     = ref<number | null>(null)
const hasMore    = ref(true)
const loading    = ref(false)
const error      = ref('')
const sentinel   = ref<HTMLElement | null>(null)

let observer: IntersectionObserver | null = null

async function loadMore() {
  if (loading.value || !hasMore.value) return
  loading.value = true
  try {
    const page = await postsApi.list({
      before: cursor.value ?? undefined,
      limit:  PAGE_SIZE,
    })
    // Dedup defensively in case the cursor edge straddles a write.
    const seen = new Set(posts.value.map(p => p.id))
    for (const p of page.posts) if (!seen.has(p.id)) posts.value.push(p)

    if (page.next_cursor != null) {
      cursor.value = page.next_cursor
    } else {
      hasMore.value = false
    }
  } catch (e: any) {
    error.value = e?.response?.data?.error ?? 'Failed to load feed'
    hasMore.value = false
  } finally {
    loading.value = false
  }
}

onMounted(async () => {
  await loadMore()
  // IntersectionObserver fires when the sentinel comes into view at the
  // bottom of the feed — that's our cue to fetch the next page.
  observer = new IntersectionObserver((entries) => {
    if (entries.some(e => e.isIntersecting)) loadMore()
  }, { rootMargin: '400px 0px' })
  if (sentinel.value) observer.observe(sentinel.value)
})

onBeforeUnmount(() => { observer?.disconnect() })
</script>

<template>
  <h1>{{ $t('feed.heading') }}</h1>

  <div v-if="error" class="card" role="alert">
    <p class="error" style="margin: 0;">{{ error }}</p>
  </div>

  <!-- First load: skeletons rather than a text line, so the page does not
       jump when the real cards replace them. -->
  <PostCardSkeleton v-if="loading && !posts.length" :count="3" />

  <div v-else-if="!posts.length" class="empty-state">
    <span class="emoji" aria-hidden="true">📝</span>
    <p>{{ $t('feed.empty') }}</p>
  </div>

  <PostCard v-for="p in posts" :key="p.id" :post="p" clamp />

  <!-- Sentinel for the infinite scroll observer. -->
  <div ref="sentinel" aria-hidden="true"></div>

  <!-- Subsequent pages: one skeleton is enough to signal "more coming"
       without implying how many. aria-live so the state is announced
       rather than only drawn. -->
  <div aria-live="polite" :aria-busy="loading">
    <PostCardSkeleton v-if="loading && posts.length" :count="1" />
  </div>

  <p v-if="!loading && !hasMore && posts.length" class="muted"
     style="text-align: center; padding-block: var(--sp-4);">
    — {{ $t('feed.heading') }} —
  </p>
</template>
