<script setup lang="ts">
import { ref, watch, onMounted } from 'vue'
import { postsApi, type Post } from '@/api/posts'
import PostCard from '@/components/PostCard.vue'
import PostCardSkeleton from '@/components/PostCardSkeleton.vue'

const props = defineProps<{ slug: string }>()

const posts   = ref<Post[]>([])
const loading = ref(true)
const error   = ref('')

async function load(slug: string) {
  loading.value = true
  error.value = ''
  try {
    const res = await postsApi.byTag(slug)
    posts.value = res.posts
  } catch (e: any) {
    // A slug that folds to nothing, or one no post carries, comes back 404.
    // That is an empty page, not an error worth an alert.
    error.value = e?.response?.status === 404
      ? ''
      : (e?.response?.data?.error ?? 'Could not load this tag')
    posts.value = []
  } finally {
    loading.value = false
  }
}

onMounted(() => load(props.slug))
watch(() => props.slug, (s) => load(s))
</script>

<template>
  <header class="tag-head">
    <p class="muted tag-eyebrow">Tagged</p>
    <h1 class="page-title tag-title">{{ slug }}</h1>
    <router-link :to="{ name: 'tags' }" class="link-action">
      <span aria-hidden="true">←</span> All tags
    </router-link>
  </header>

  <template v-if="loading">
    <p class="visually-hidden" role="status">Loading posts…</p>
    <PostCardSkeleton :count="3" />
  </template>

  <div v-else-if="error" class="empty-state" role="alert">
    <span class="emoji" aria-hidden="true">⚠️</span>
    <p class="error">{{ error }}</p>
  </div>

  <div v-else-if="!posts.length" class="empty-state">
    <span class="emoji" aria-hidden="true">🏷️</span>
    <p>Nothing is tagged “{{ slug }}” yet.</p>
    <router-link :to="{ name: 'tags' }" class="btn ghost">Browse all tags</router-link>
  </div>

  <template v-else>
    <p class="muted count-line">
      {{ posts.length }} post{{ posts.length === 1 ? '' : 's' }}
    </p>
    <PostCard v-for="p in posts" :key="p.id" :post="p" clamp />
  </template>
</template>

<style scoped>
.tag-head { margin-bottom: var(--sp-5); }
.tag-eyebrow { margin: 0; text-transform: uppercase; letter-spacing: 0.08em; }
.tag-title { margin: 0 0 var(--sp-2); overflow-wrap: anywhere; }
.count-line { margin-bottom: var(--sp-4); }
</style>
