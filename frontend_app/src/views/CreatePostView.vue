<script setup lang="ts">
import { ref, computed, watch, onMounted, onBeforeUnmount, nextTick } from 'vue'
import { useRouter } from 'vue-router'
import { postsApi } from '@/api/posts'
import { useToastStore } from '@/stores/toast'
import { sanitizePostHtml } from '@/lib/sanitize'

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

// --- Live preview --------------------------------------------------------
//
// Rendered by the server, through the same cmark-gfm SAFE pipeline that
// publishing uses. A second, client-side markdown renderer would have been
// one fewer round trip and a different set of bugs: every place the two
// disagreed, the preview would be quietly lying about what the post is
// going to look like. The response still goes through sanitizePostHtml,
// exactly like a published body does.
//
// Above 900 px the panes sit side by side and update as you type; below it
// they are tabs, because two 340 px columns are neither a usable editor nor
// a readable preview.

const previewHtml    = ref('')
const previewMinutes = ref(0)
const previewBusy    = ref(false)
const previewError   = ref('')
const previewEl      = ref<HTMLElement | null>(null)

const mode   = ref<'write' | 'preview'>('write')
const isWide = ref(false)

const previewWanted = computed(() => isWide.value || mode.value === 'preview')

let media: MediaQueryList | null = null
function onMediaChange(e: MediaQueryListEvent | MediaQueryList) {
  isWide.value = e.matches
}

let debounceTimer: ReturnType<typeof setTimeout> | undefined
let inflight: AbortController | null = null
// What the current preview was rendered from, so switching tabs back and
// forth on unchanged text does not spend a request each time. null rather
// than '' so an untouched editor still counts as "nothing rendered yet".
let rendered: string | null = null

function schedulePreview(now = false) {
  if (!previewWanted.value) return
  if (content.value === rendered) return
  clearTimeout(debounceTimer)
  // ~500 ms is a typing pause rather than a gap between keystrokes, which
  // keeps this to roughly one request per thought and well inside the
  // endpoint's rate limit.
  debounceTimer = setTimeout(runPreview, now ? 0 : 500)
}

async function runPreview() {
  const text = content.value
  if (!text.trim()) {
    previewHtml.value = ''
    previewMinutes.value = 0
    previewError.value = ''
    rendered = text
    return
  }

  // Abort whatever is still in the air: an earlier, shorter draft landing
  // after a later one would show the author the wrong text.
  inflight?.abort()
  const ac = new AbortController()
  inflight = ac
  previewBusy.value = true

  try {
    const res = await postsApi.preview(text, ac.signal)
    previewHtml.value    = sanitizePostHtml(res.content_html)
    previewMinutes.value = res.reading_minutes
    previewError.value   = ''
    rendered = text
    await nextTick()
    const { highlightWithin } = await import('@/lib/highlight')
    highlightWithin(previewEl.value)
  } catch (e: any) {
    if (ac.signal.aborted || e?.code === 'ERR_CANCELED') return
    previewError.value = e?.response?.status === 429
      ? 'Preview paused — too many renders in a row. Keep writing; it will catch up.'
      : (e?.response?.data?.error ?? 'Could not render the preview')
  } finally {
    if (inflight === ac) {
      inflight = null
      previewBusy.value = false
    }
  }
}

watch(content, () => schedulePreview())
watch(previewWanted, (wanted) => { if (wanted) schedulePreview(true) })

onMounted(() => {
  media = window.matchMedia('(min-width: 900px)')
  onMediaChange(media)
  media.addEventListener('change', onMediaChange)
})
onBeforeUnmount(() => {
  media?.removeEventListener('change', onMediaChange)
  clearTimeout(debounceTimer)
  inflight?.abort()
})

// --- Save ----------------------------------------------------------------

async function save(asDraft: boolean) {
  error.value = ''
  if (!title.value.trim() || !content.value.trim()) {
    error.value = 'A post needs a title and a body.'
    return
  }
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

    <!-- Tabs only when the panes cannot both be on screen. Above 900 px
         both are visible, so a control that switches between them would be
         switching nothing. -->
    <div v-if="!isWide" class="tabs editor-tabs" role="tablist" aria-label="Editor view">
      <button
        type="button" role="tab" :aria-selected="mode === 'write'"
        :class="{ active: mode === 'write' }"
        @click="mode = 'write'"
      >Write</button>
      <button
        type="button" role="tab" :aria-selected="mode === 'preview'"
        :class="{ active: mode === 'preview' }"
        @click="mode = 'preview'"
      >Preview</button>
    </div>

    <div class="editor-panes" :class="{ split: isWide }">
      <div v-show="isWide || mode === 'write'" class="pane">
        <label for="post-content">Content</label>
        <!-- No `required` here on purpose: while the Preview tab is showing,
             this textarea is display:none, and a hidden required control
             makes the browser refuse to submit the form with an error the
             author never sees. save() checks it instead. -->
        <textarea id="post-content" ref="contentEl" v-model="content" rows="16"
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
      </div>

      <div v-show="isWide || mode === 'preview'" class="pane preview-pane">
        <div class="preview-head">
          <span class="preview-label">Preview</span>
          <span v-if="previewMinutes" class="muted">{{ previewMinutes }} min read</span>
          <span class="spacer"></span>
          <span v-if="previewBusy" class="muted preview-busy">rendering…</span>
        </div>

        <p v-if="previewError" class="muted preview-note" role="status">{{ previewError }}</p>

        <!-- Server-rendered by cmark-gfm in SAFE mode and passed through the
             same client-side sanitizer a published body gets. -->
        <div v-if="previewHtml" ref="previewEl" class="post-body preview-body" v-html="previewHtml"></div>
        <p v-else class="muted preview-empty">Nothing to preview yet — start writing on the left.</p>
      </div>
    </div>

    <label for="post-tags">Tags</label>
    <input id="post-tags" v-model="tagsInput" maxlength="200"
           placeholder="rust, databases, performance"
           aria-describedby="post-tags-hint" />
    <p id="post-tags-hint" class="muted" style="margin-top: var(--sp-2);">
      Comma-separated, up to 8. “C++” and “c++” are the same tag.
    </p>

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

<style scoped>
.editor-tabs { margin: var(--sp-4) 0 var(--sp-3); }

.editor-panes.split {
  display: grid;
  grid-template-columns: repeat(2, minmax(0, 1fr));
  gap: var(--sp-4);
  align-items: start;
}

.pane { min-width: 0; }

.preview-pane {
  display: flex;
  flex-direction: column;
  min-height: 12rem;
}

.preview-head {
  display: flex;
  align-items: center;
  gap: var(--sp-2);
  margin-bottom: var(--sp-2);
  font-size: var(--step--1);
}
.preview-label {
  font-weight: 600;
  text-transform: uppercase;
  letter-spacing: 0.06em;
  color: var(--text-dim);
}
.preview-busy { font-style: italic; }

.preview-note { margin: 0 0 var(--sp-2); }

.preview-body,
.preview-empty {
  flex: 1;
  padding: var(--sp-4);
  background: var(--bg-inset);
  border: 1px solid var(--border);
  border-radius: var(--radius);
  /* Matches the textarea, so the two panes line up instead of one growing
     the page every time a paragraph is added. */
  max-height: 34rem;
  overflow-y: auto;
}
.preview-empty { margin: 0; display: flex; align-items: center; justify-content: center; }

/* The tabbed layout has nothing beside it, so the cap only makes sense when
   the panes are actually side by side. */
.editor-panes:not(.split) .preview-body { max-height: none; }
</style>
