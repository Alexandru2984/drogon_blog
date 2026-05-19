<script setup lang="ts">
import { ref } from 'vue'
import { useRouter } from 'vue-router'
import { postsApi } from '@/api/posts'
import { useToastStore } from '@/stores/toast'

const title = ref('')
const content = ref('')
const loading = ref(false)
const error = ref('')

const router = useRouter()
const toasts = useToastStore()

async function submit() {
  error.value = ''
  loading.value = true
  try {
    const res = await postsApi.create({ title: title.value, content: content.value })
    toasts.push('Post published', 'ok')
    router.push({ name: 'post', params: { id: res.post.id } })
  } catch (e: any) {
    error.value = e?.response?.data?.error ?? 'Failed to publish'
  } finally {
    loading.value = false
  }
}
</script>

<template>
  <h1>New post</h1>
  <form @submit.prevent="submit" class="card">
    <label>Title</label>
    <input v-model="title" required maxlength="200" />
    <label>Content</label>
    <textarea v-model="content" required rows="12"></textarea>
    <p v-if="error" class="error">{{ error }}</p>
    <div class="toolbar" style="margin-top: 1rem;">
      <button :disabled="loading || !title || !content">
        {{ loading ? 'Publishing…' : 'Publish' }}
      </button>
      <router-link to="/" class="btn ghost">Cancel</router-link>
    </div>
  </form>
</template>
