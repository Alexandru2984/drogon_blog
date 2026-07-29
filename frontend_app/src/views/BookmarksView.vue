<script setup lang="ts">
import { ref, onMounted } from 'vue'
import { socialApi } from '@/api/social'
import type { Post } from '@/api/posts'
import PostCard from '@/components/PostCard.vue'
import PostCardSkeleton from '@/components/PostCardSkeleton.vue'

const posts   = ref<Post[]>([])
const loading = ref(true)
const error   = ref('')

onMounted(async () => {
  try {
    posts.value = await socialApi.bookmarks()
  } catch (e: any) {
    error.value = e?.response?.data?.error ?? 'Could not load your bookmarks'
  } finally {
    loading.value = false
  }
})
</script>

<template>
  <h1 class="page-title">Saved</h1>

  <template v-if="loading">
    <p class="visually-hidden" role="status">Loading saved posts…</p>
    <PostCardSkeleton :count="3" />
  </template>

  <div v-else-if="error" class="empty-state" role="alert">
    <span class="emoji" aria-hidden="true">⚠️</span>
    <p class="error">{{ error }}</p>
  </div>

  <div v-else-if="!posts.length" class="empty-state">
    <span class="emoji" aria-hidden="true">🔖</span>
    <p>Nothing saved yet. Use the bookmark button on a post to keep it here.</p>
    <router-link to="/" class="btn ghost">Browse the feed</router-link>
  </div>

  <PostCard v-for="p in posts" :key="p.id" :post="p" clamp />
</template>
