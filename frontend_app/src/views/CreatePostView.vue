<script setup lang="ts">
import { ref } from 'vue'
import { useRouter } from 'vue-router'
import { postsApi } from '@/api/posts'
import { useToastStore } from '@/stores/toast'

const title = ref('')
const content = ref('')
const tagsInput = ref('')
const loading = ref(false)
const savingDraft = ref(false)
const error = ref('')

// Split on commas here rather than sending the raw string, so what the
// author sees in the field is what gets sent. The server normalises and
// deduplicates regardless — this is only about the payload being honest.
function tagList(): string[] {
  return tagsInput.value
    .split(',')
    .map(t => t.trim())
    .filter(Boolean)
}

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

async function save(asDraft: boolean) {
  error.value = ''
  if (asDraft) savingDraft.value = true
  else         loading.value = true
  try {
    const res = await postsApi.create({
      title:   title.value,
      content: content.value,
      tags:    tagList(),
      draft:   asDraft,
    })
    toasts.push(asDraft ? 'Draft saved' : 'Post published', 'ok')
    // A draft goes to the drafts list, where the author can keep working;
    // a published post goes to the post, which is what they just made.
    router.push(asDraft
      ? { name: 'drafts' }
      : { name: 'post', params: { id: res.post.id } })
  } catch (e: any) {
    error.value = e?.response?.data?.error ?? (asDraft ? 'Failed to save' : 'Failed to publish')
  } finally {
    loading.value = false
    savingDraft.value = false
  }
}

function submit() { save(false) }
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

    <label for="post-tags">Tags</label>
    <input id="post-tags" v-model="tagsInput" maxlength="200"
           placeholder="rust, databases, performance"
           aria-describedby="post-tags-hint" />
    <p id="post-tags-hint" class="muted" style="margin-top: var(--sp-2);">
      Comma-separated, up to 8. “C++” and “c++” are the same tag.
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
      <button :disabled="loading || savingDraft || !title.trim() || !content.trim()">
        {{ loading ? 'Publishing…' : 'Publish' }}
      </button>
      <!-- type=button so it does not submit the form: saving a draft is a
           different action, not a variant of publishing. -->
      <button type="button" class="ghost"
              :disabled="loading || savingDraft || !title.trim() || !content.trim()"
              @click="save(true)">
        {{ savingDraft ? 'Saving…' : 'Save as draft' }}
      </button>
      <router-link to="/" class="btn quiet">Cancel</router-link>
    </div>
  </form>
</template>
