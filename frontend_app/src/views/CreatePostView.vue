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

// Inline image upload: send the file to the server, which re-encodes + strips
// metadata and returns a same-origin URL, then splice the Markdown image
// syntax into the content at the caret.
const contentEl = ref<HTMLTextAreaElement | null>(null)
const imageInput = ref<HTMLInputElement | null>(null)
const uploadingImage = ref(false)

function insertAtCaret(snippet: string) {
  const el = contentEl.value
  if (!el) {
    content.value += snippet
    return
  }
  const start = el.selectionStart ?? content.value.length
  const end = el.selectionEnd ?? content.value.length
  content.value = content.value.slice(0, start) + snippet + content.value.slice(end)
  // Restore caret after the inserted text on next tick.
  requestAnimationFrame(() => {
    el.focus()
    const pos = start + snippet.length
    el.setSelectionRange(pos, pos)
  })
}

async function onImagePicked(ev: Event) {
  const input = ev.target as HTMLInputElement
  const file = input.files?.[0]
  if (!file) return
  uploadingImage.value = true
  error.value = ''
  try {
    const url = await postsApi.uploadImage(file)
    insertAtCaret(`\n![](${url})\n`)
    toasts.push('Image uploaded', 'ok')
  } catch (e: any) {
    error.value = e?.response?.data?.error ?? 'Image upload failed'
  } finally {
    uploadingImage.value = false
    input.value = '' // allow re-picking the same file
  }
}

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
    <label for="post-title">Title</label>
    <input id="post-title" v-model="title" required maxlength="200" />
    <label for="post-content">Content</label>
    <textarea id="post-content" ref="contentEl" v-model="content" required rows="12"></textarea>
    <div class="toolbar" style="margin-top: 0.5rem;">
      <button type="button" class="btn ghost"
              :disabled="uploadingImage"
              @click="imageInput?.click()">
        {{ uploadingImage ? 'Uploading…' : '🖼 Insert image' }}
      </button>
      <input ref="imageInput" type="file" accept="image/jpeg,image/png,image/webp"
             style="display: none" @change="onImagePicked" />
      <span class="muted" style="font-size: 0.85em;">JPEG / PNG / WebP, up to 8 MB</span>
    </div>
    <p v-if="error" class="error">{{ error }}</p>
    <div class="toolbar" style="margin-top: 1rem;">
      <button :disabled="loading || !title || !content">
        {{ loading ? 'Publishing…' : 'Publish' }}
      </button>
      <router-link to="/" class="btn ghost">Cancel</router-link>
    </div>
  </form>
</template>
