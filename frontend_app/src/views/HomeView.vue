<script setup lang="ts">
import { ref, onMounted } from 'vue'
import { postsApi, type Post } from '@/api/posts'
import PostCard from '@/components/PostCard.vue'

const posts = ref<Post[]>([])
const loading = ref(true)
const error = ref('')

onMounted(async () => {
  try {
    posts.value = await postsApi.list()
  } catch (e: any) {
    error.value = e?.response?.data?.error ?? 'Failed to load feed'
  } finally {
    loading.value = false
  }
})
</script>

<template>
  <h1>Feed</h1>
  <p v-if="loading" class="muted">Loading…</p>
  <p v-else-if="error" class="error">{{ error }}</p>
  <p v-else-if="!posts.length" class="muted">No posts yet. Be the first to write one!</p>
  <PostCard v-for="p in posts" :key="p.id" :post="p" clamp />
</template>
