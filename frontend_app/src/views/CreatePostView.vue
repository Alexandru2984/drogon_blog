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
  <h1 class="page-title">New post</h1>
  <form @submit.prevent="submit" class="card">
    <label for="post-title">Title</label>
    <input id="post-title" v-model="title" required maxlength="200" />

    <label for="post-content">Content</label>
    <textarea id="post-content" ref="contentEl" v-model="content" required rows="12"
              aria-describedby="post-content-hint"></textarea>
    <p id="post-content-hint" class="muted" style="margin-top: var(--sp-2);">
      Markdown is supported — headings, lists, links, tables and fenced code.
    </p>

    <div class="row tight" style="margin-top: var(--sp-3);">
      <button type="button" class="ghost"
              :disabled="uploadingImage"
              @click="imageInput?.click()">
        <span aria-hidden="true">🖼</span>
        {{ uploadingImage ? 'Uploading…' : 'Insert image' }}
      </button>
      <!-- display:none, not .visually-hidden: the button above is the
           labelled control and this input is only ever clicked
           programmatically, so it should stay out of the accessibility
           tree entirely rather than become a second, unlabelled one. -->
      <input ref="imageInput" type="file" accept="image/jpeg,image/png,image/webp"
             style="display: none" @change="onImagePicked" />
      <span class="muted">JPEG / PNG / WebP, up to 8 MB</span>
    </div>

    <p v-if="error" class="error" role="alert" style="margin-top: var(--sp-3);">{{ error }}</p>

    <!-- The two actions wrap onto separate rows rather than shrinking; at
         320 px "Publishing…" and "Cancel" side by side clipped both. -->
    <div class="row tight" style="margin-top: var(--sp-5);">
      <button :disabled="loading || !title.trim() || !content.trim()">
        {{ loading ? 'Publishing…' : 'Publish' }}
      </button>
      <router-link to="/" class="btn ghost">Cancel</router-link>
    </div>
  </form>
</template>
