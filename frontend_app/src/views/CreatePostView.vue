<script setup lang="ts">
import { ref, computed, watch, onMounted, onBeforeUnmount, nextTick } from 'vue'
import { useRouter, onBeforeRouteLeave } from 'vue-router'
import { postsApi, type Post } from '@/api/posts'
import { useToastStore } from '@/stores/toast'
import { useAuthStore } from '@/stores/auth'
import { sanitizePostHtml } from '@/lib/sanitize'
import { formatDateTime, toDatetimeLocalValue } from '@/lib/date'

// One view, two jobs: `/posts/new` (no editId) and `/posts/:id/edit`.
const props = defineProps<{ editId?: number }>()
const isEdit = computed(() => typeof props.editId === 'number' && props.editId > 0)

const router = useRouter()
const toasts = useToastStore()
const auth   = useAuthStore()

const title      = ref('')
const content    = ref('')
const tagsInput  = ref('')
const scheduleAt = ref('')   // datetime-local value, converted to UTC ISO on submit
const loading     = ref(false)
const savingDraft = ref(false)
const error       = ref('')

// Edit mode state
const original    = ref<Post | null>(null)
const loadingPost = ref(false)
const loadError   = ref('')
const isDraftPost = computed(() => !isEdit.value || original.value?.is_draft === true)

const MAX_TAGS = 8
const MAX_TITLE = 200

function tagList(): string[] {
  return tagsInput.value.split(',').map(t => t.trim()).filter(Boolean)
}
const parsedTags = computed(() => tagList())

// --- Stats ----------------------------------------------------------------
const wordCount = computed(() => content.value.trim() ? content.value.trim().split(/\s+/).length : 0)
const charCount = computed(() => content.value.length)
const estMinutes = computed(() => Math.max(1, Math.round(wordCount.value / 220)))

// --- Dirty tracking + local autosave ---------------------------------------
const snapshot = () => JSON.stringify([title.value, content.value, tagsInput.value, scheduleAt.value])
const baseline = ref(snapshot())
const dirty    = computed(() => snapshot() !== baseline.value)
let   leavingAfterSave = false

const storageKey = computed(() => isEdit.value ? `blog:editor:edit:${props.editId}` : 'blog:editor:new')

interface StoredDraft { title: string; content: string; tags: string; savedAt: number }
const recoverable = ref<StoredDraft | null>(null)

function readStored(): StoredDraft | null {
  try {
    const raw = localStorage.getItem(storageKey.value)
    if (!raw) return null
    const v = JSON.parse(raw)
    if (typeof v?.title !== 'string' || typeof v?.content !== 'string') return null
    return { title: v.title, content: v.content, tags: String(v.tags ?? ''), savedAt: Number(v.savedAt) || Date.now() }
  } catch { return null }
}
function clearStored() {
  try { localStorage.removeItem(storageKey.value) } catch { /* ignore */ }
}
let autosaveTimer: ReturnType<typeof setTimeout> | undefined
watch([title, content, tagsInput], () => {
  if (!dirty.value) return
  clearTimeout(autosaveTimer)
  autosaveTimer = setTimeout(() => {
    if (!title.value.trim() && !content.value.trim()) { clearStored(); return }
    try {
      localStorage.setItem(storageKey.value, JSON.stringify({
        title: title.value, content: content.value, tags: tagsInput.value, savedAt: Date.now(),
      }))
    } catch { /* quota / private mode: autosave is best-effort */ }
  }, 800)
})

function offerRecovery() {
  const s = readStored()
  if (!s) return
  if (s.title === title.value && s.content === content.value && s.tags === tagsInput.value) {
    clearStored()
    return
  }
  if (!s.title.trim() && !s.content.trim()) return
  recoverable.value = s
}
function restoreRecovered() {
  if (!recoverable.value) return
  title.value     = recoverable.value.title
  content.value   = recoverable.value.content
  tagsInput.value = recoverable.value.tags
  recoverable.value = null
  toasts.push('Unsaved changes restored', 'ok')
}
function discardRecovered() {
  recoverable.value = null
  clearStored()
}

function onBeforeUnload(e: BeforeUnloadEvent) {
  if (dirty.value && !leavingAfterSave) {
    e.preventDefault()
    e.returnValue = ''
  }
}
onBeforeRouteLeave(() => {
  if (dirty.value && !leavingAfterSave) {
    return window.confirm('You have unsaved changes. Leave anyway? (A local copy is kept for recovery.)')
  }
})

// --- Load existing post (edit mode) ----------------------------------------
async function loadForEdit() {
  if (!isEdit.value) return
  loadingPost.value = true
  loadError.value = ''
  try {
    const p = await postsApi.get(props.editId!)
    if (auth.user && p.author && p.author.id !== auth.user.id) {
      loadError.value = 'You can only edit your own posts.'
      return
    }
    original.value  = p
    title.value     = p.title
    content.value   = p.content
    tagsInput.value = (p.tags ?? []).map(t => t.label).join(', ')
    scheduleAt.value = p.is_draft && p.scheduled_at ? toDatetimeLocalValue(p.scheduled_at) : ''
    baseline.value  = snapshot()
    offerRecovery()
  } catch (e: any) {
    loadError.value = e?.response?.status === 404
      ? 'This post does not exist or you cannot edit it.'
      : (e?.response?.data?.error ?? 'Could not load the post')
  } finally {
    loadingPost.value = false
  }
}

// --- Markdown toolbar --------------------------------------------------------
const contentEl  = ref<HTMLTextAreaElement | null>(null)
const imageInput = ref<HTMLInputElement | null>(null)
const uploadingImage = ref(0)
const dragOver = ref(false)

function setSelection(start: number, end: number) {
  const el = contentEl.value
  if (!el) return
  requestAnimationFrame(() => {
    el.focus()
    el.setSelectionRange(start, end)
  })
}

function insertAtCaret(snippet: string) {
  const el = contentEl.value
  const start = el?.selectionStart ?? content.value.length
  const end   = el?.selectionEnd ?? content.value.length
  content.value = content.value.slice(0, start) + snippet + content.value.slice(end)
  const pos = start + snippet.length
  setSelection(pos, pos)
}

function wrapSelection(before: string, after = before, placeholder = 'text') {
  const el = contentEl.value
  const start = el?.selectionStart ?? content.value.length
  const end   = el?.selectionEnd ?? content.value.length
  const selected = content.value.slice(start, end) || placeholder
  content.value = content.value.slice(0, start) + before + selected + after + content.value.slice(end)
  setSelection(start + before.length, start + before.length + selected.length)
}

function prefixLines(prefix: string | ((i: number) => string)) {
  const el = contentEl.value
  const start = el?.selectionStart ?? content.value.length
  const end   = el?.selectionEnd ?? content.value.length
  const lineStart = content.value.lastIndexOf('\n', start - 1) + 1
  const nl = content.value.indexOf('\n', end)
  const lineEnd = nl === -1 ? content.value.length : nl
  const block = content.value.slice(lineStart, lineEnd)
  const out = block.split('\n').map((l, i) => (typeof prefix === 'function' ? prefix(i) : prefix) + l).join('\n')
  content.value = content.value.slice(0, lineStart) + out + content.value.slice(lineEnd)
  setSelection(lineStart, lineStart + out.length)
}

function insertBlock(block: string) {
  const el = contentEl.value
  const start = el?.selectionStart ?? content.value.length
  const needsLead = start > 0 && content.value[start - 1] !== '\n'
  insertAtCaret((needsLead ? '\n\n' : '') + block + '\n')
}

const tools = [
  { id: 'bold',   label: 'Bold (Ctrl+B)',     icon: 'B',   run: () => wrapSelection('**', '**', 'bold text') },
  { id: 'italic', label: 'Italic (Ctrl+I)',   icon: 'I',   run: () => wrapSelection('_', '_', 'italic text') },
  { id: 'strike', label: 'Strikethrough',     icon: 'S',   run: () => wrapSelection('~~', '~~', 'struck') },
  { id: 'h2',     label: 'Heading',           icon: 'H',   run: () => prefixLines('## ') },
  { id: 'quote',  label: 'Quote',             icon: '❝',   run: () => prefixLines('> ') },
  { id: 'code',   label: 'Inline code',       icon: '</>', run: () => wrapSelection('`', '`', 'code') },
  { id: 'fence',  label: 'Code block',        icon: '{ }', run: () => wrapSelection('\n```\n', '\n```\n', 'code here') },
  { id: 'link',   label: 'Link (Ctrl+K)',     icon: '🔗',  run: () => insertLink() },
  { id: 'ul',     label: 'Bulleted list',     icon: '•',   run: () => prefixLines('- ') },
  { id: 'ol',     label: 'Numbered list',     icon: '1.',  run: () => prefixLines(i => `${i + 1}. `) },
  { id: 'task',   label: 'Task list',         icon: '☑',   run: () => prefixLines('- [ ] ') },
  { id: 'table',  label: 'Table',             icon: '▦',   run: () => insertBlock('| Column | Column |\n| ------ | ------ |\n| Cell   | Cell   |') },
  { id: 'hr',     label: 'Divider',           icon: '―',   run: () => insertBlock('---') },
] as const

function insertLink() {
  const el = contentEl.value
  const start = el?.selectionStart ?? content.value.length
  const end   = el?.selectionEnd ?? content.value.length
  const text = content.value.slice(start, end) || 'link text'
  const snippet = `[${text}](https://)`
  content.value = content.value.slice(0, start) + snippet + content.value.slice(end)
  const urlStart = start + text.length + 3
  setSelection(urlStart, urlStart + 8)
}

function onEditorKeydown(e: KeyboardEvent) {
  const mod = e.ctrlKey || e.metaKey
  if (mod && !e.shiftKey && !e.altKey) {
    const k = e.key.toLowerCase()
    if (k === 'b') { e.preventDefault(); wrapSelection('**', '**', 'bold text'); return }
    if (k === 'i') { e.preventDefault(); wrapSelection('_', '_', 'italic text'); return }
    if (k === 'k') { e.preventDefault(); insertLink(); return }
    if (k === 's') { e.preventDefault(); if (canSave.value) save(isDraftPost.value); return }
    if (k === 'enter') { e.preventDefault(); if (canSave.value) submit(); return }
  }
  // Tab indents inside the editor instead of leaving it (Esc then Tab still exits).
  if (e.key === 'Tab' && !mod && !e.altKey && !e.shiftKey) {
    e.preventDefault()
    insertAtCaret('  ')
  }
}

// --- Images: picker, paste, drag & drop --------------------------------------
const ALLOWED_IMAGES = ['image/jpeg', 'image/png', 'image/webp']
const MAX_IMAGE_BYTES = 8 * 1024 * 1024

async function uploadFiles(files: File[]) {
  const images = files.filter(f => ALLOWED_IMAGES.includes(f.type))
  if (!images.length) {
    if (files.length) error.value = 'Only JPEG, PNG and WebP images can be inserted.'
    return
  }
  error.value = ''
  for (const file of images) {
    if (file.size > MAX_IMAGE_BYTES) {
      toasts.push(`${file.name} is larger than 8 MB`, 'error')
      continue
    }
    uploadingImage.value++
    try {
      const url = await postsApi.uploadImage(file)
      const alt = file.name.replace(/\.[a-z0-9]+$/i, '').replace(/[[\]()\n\r]/g, '').slice(0, 80)
      insertAtCaret(`\n![${alt}](${url})\n`)
    } catch (e: any) {
      error.value = e?.response?.data?.error ?? 'Image upload failed'
    } finally {
      uploadingImage.value--
    }
  }
  if (!error.value) toasts.push(images.length > 1 ? 'Images uploaded' : 'Image uploaded', 'ok')
}

async function onImagePicked(ev: Event) {
  const input = ev.target as HTMLInputElement
  const files = Array.from(input.files ?? [])
  input.value = ''
  await uploadFiles(files)
}
function onPaste(e: ClipboardEvent) {
  const files = Array.from(e.clipboardData?.files ?? []).filter(f => f.type.startsWith('image/'))
  if (!files.length) return
  e.preventDefault()
  uploadFiles(files)
}
function onDrop(e: DragEvent) {
  dragOver.value = false
  const files = Array.from(e.dataTransfer?.files ?? [])
  if (!files.length) return
  e.preventDefault()
  uploadFiles(files)
}
function onDragOver(e: DragEvent) {
  if (e.dataTransfer?.types?.includes('Files')) {
    e.preventDefault()
    dragOver.value = true
  }
}

// --- Live preview (server-rendered, same pipeline as publishing) ------------
const previewHtml    = ref('')
const previewMinutes = ref(0)
const previewBusy    = ref(false)
const previewError   = ref('')
const previewEl      = ref<HTMLElement | null>(null)

const mode   = ref<'write' | 'preview'>('write')
const isWide = ref(false)
const previewWanted = computed(() => isWide.value || mode.value === 'preview')

let media: MediaQueryList | null = null
function onMediaChange(e: MediaQueryListEvent | MediaQueryList) { isWide.value = e.matches }

let debounceTimer: ReturnType<typeof setTimeout> | undefined
let inflight: AbortController | null = null
let rendered: string | null = null

function schedulePreview(now = false) {
  if (!previewWanted.value) return
  if (content.value === rendered) return
  clearTimeout(debounceTimer)
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

onMounted(async () => {
  media = window.matchMedia('(min-width: 900px)')
  onMediaChange(media)
  media.addEventListener('change', onMediaChange)
  window.addEventListener('beforeunload', onBeforeUnload)
  if (isEdit.value) await loadForEdit()
  else offerRecovery()
})
onBeforeUnmount(() => {
  media?.removeEventListener('change', onMediaChange)
  window.removeEventListener('beforeunload', onBeforeUnload)
  clearTimeout(debounceTimer)
  clearTimeout(autosaveTimer)
  inflight?.abort()
})

// --- Save --------------------------------------------------------------------
const minSchedule = computed(() => toDatetimeLocalValue(new Date(Date.now() + 60_000).toISOString()))
const canSave = computed(() =>
  !loading.value && !savingDraft.value && !loadingPost.value && !loadError.value &&
  !!title.value.trim() && !!content.value.trim() && uploadingImage.value === 0)

function validate(): boolean {
  if (!title.value.trim() || !content.value.trim()) {
    error.value = 'A post needs a title and a body.'
    return false
  }
  if (parsedTags.value.length > MAX_TAGS) {
    error.value = `Use at most ${MAX_TAGS} tags.`
    return false
  }
  if (scheduleAt.value && isDraftPost.value) {
    const when = new Date(scheduleAt.value)
    if (Number.isNaN(when.getTime()) || when.getTime() <= Date.now()) {
      error.value = 'The scheduled time must be in the future.'
      return false
    }
  }
  return true
}

function finishAndGo(target: Parameters<typeof router.push>[0]) {
  clearStored()
  leavingAfterSave = true
  baseline.value = snapshot()
  router.push(target)
}

// asDraft: in "new" mode and for an existing draft this saves without
// publishing. For a published post the secondary action is "Move to drafts".
async function save(asDraft: boolean) {
  error.value = ''
  if (!validate()) return
  if (asDraft) savingDraft.value = true
  else         loading.value = true
  const scheduled = !asDraft && isDraftPost.value && !!scheduleAt.value
  const scheduleIso = scheduleAt.value ? new Date(scheduleAt.value).toISOString() : null
  try {
    if (!isEdit.value) {
      const payload: { title: string; content: string; tags: string[]; draft: boolean; publish_at?: string } = {
        title: title.value, content: content.value, tags: tagList(), draft: asDraft,
      }
      if (scheduled && scheduleIso) payload.publish_at = scheduleIso
      const res = await postsApi.create(payload)
      toasts.push(scheduled ? 'Post scheduled' : asDraft ? 'Draft saved' : 'Post published', 'ok')
      finishAndGo(scheduled || asDraft ? { name: 'drafts' } : { name: 'post', params: { id: res.post.id } })
      return
    }

    const id = props.editId!
    const payload: {
      title: string; content: string; tags: string[]; draft?: boolean; publish_at?: string | null
    } = { title: title.value, content: content.value, tags: tagList() }

    if (isDraftPost.value) {
      if (asDraft) {
        // Keep it a draft; keep / set / clear the schedule as the form says.
        payload.publish_at = scheduleIso
      } else if (scheduled) {
        payload.publish_at = scheduleIso
      } else {
        payload.publish_at = null
        payload.draft = false
      }
    } else if (asDraft) {
      payload.draft = true
    }

    const res = await postsApi.update(id, payload)
    const nowDraft = res?.post?.is_draft ?? (asDraft || scheduled)
    toasts.push(
      scheduled ? 'Post scheduled'
      : !isDraftPost.value && asDraft ? 'Moved to drafts'
      : isDraftPost.value && !asDraft ? 'Post published'
      : 'Changes saved', 'ok')
    finishAndGo(nowDraft ? { name: 'drafts' } : { name: 'post', params: { id } })
  } catch (e: any) {
    error.value = e?.response?.data?.error
      ?? (scheduled ? 'Failed to schedule' : asDraft ? 'Failed to save' : 'Failed to publish')
  } finally {
    loading.value = false
    savingDraft.value = false
  }
}

function submit() { save(false) }

const primaryLabel = computed(() => {
  if (loading.value) {
    if (isEdit.value && !isDraftPost.value) return 'Saving…'
    return scheduleAt.value ? 'Scheduling…' : 'Publishing…'
  }
  if (isEdit.value && !isDraftPost.value) return 'Save changes'
  return scheduleAt.value ? 'Schedule' : 'Publish'
})
const secondaryLabel = computed(() => {
  if (savingDraft.value) return 'Saving…'
  if (isEdit.value && !isDraftPost.value) return 'Move to drafts'
  return isEdit.value ? 'Save draft' : 'Save as draft'
})
const cancelTarget = computed(() =>
  isEdit.value
    ? (isDraftPost.value ? { name: 'drafts' } : { name: 'post', params: { id: props.editId } })
    : { name: 'home' })
</script>

<template>
  <h1 class="page-title">{{ isEdit ? (isDraftPost ? 'Edit draft' : 'Edit post') : 'New post' }}</h1>

  <div v-if="loadingPost" class="card" aria-busy="true">
    <div class="skeleton line" style="height: 2.4em;"></div>
    <div class="skeleton line" style="height: 14em; margin-top: var(--sp-4);"></div>
  </div>

  <div v-else-if="loadError" class="empty-state" role="alert">
    <span class="emoji" aria-hidden="true">🔒</span>
    <p class="error">{{ loadError }}</p>
    <router-link to="/drafts" class="btn ghost">Go to drafts</router-link>
  </div>

  <form v-else @submit.prevent="submit" class="card editor-card">
    <div v-if="recoverable" class="recover-banner" role="status">
      <span>
        <strong>Unsaved changes found</strong>
        <span class="muted"> from {{ new Date(recoverable.savedAt).toLocaleString() }}</span>
      </span>
      <span class="row tight">
        <button type="button" class="sm" @click="restoreRecovered">Restore</button>
        <button type="button" class="sm ghost" @click="discardRecovered">Discard</button>
      </span>
    </div>

    <p v-if="isEdit && original?.is_draft && original?.scheduled_at" class="muted schedule-note">
      ⏰ Currently scheduled for {{ formatDateTime(original.scheduled_at) }}
    </p>

    <label for="post-title">Title</label>
    <input id="post-title" v-model="title" required :maxlength="MAX_TITLE" autocomplete="off" />
    <p class="muted counter" aria-live="off">{{ title.length }} / {{ MAX_TITLE }}</p>

    <div v-if="!isWide" class="tabs editor-tabs" role="tablist" aria-label="Editor view">
      <button type="button" role="tab" :aria-selected="mode === 'write'"
              :class="{ active: mode === 'write' }" @click="mode = 'write'">Write</button>
      <button type="button" role="tab" :aria-selected="mode === 'preview'"
              :class="{ active: mode === 'preview' }" @click="mode = 'preview'">Preview</button>
    </div>

    <div class="editor-panes" :class="{ split: isWide }">
      <div v-show="isWide || mode === 'write'" class="pane">
        <label for="post-content">Content</label>

        <div class="md-toolbar" role="toolbar" aria-label="Formatting">
          <button v-for="t in tools" :key="t.id" type="button" class="md-tool" :class="`md-${t.id}`"
                  :title="t.label" :aria-label="t.label" @mousedown.prevent @click="t.run()">
            <span aria-hidden="true">{{ t.icon }}</span>
          </button>
          <span class="md-sep" aria-hidden="true"></span>
          <button type="button" class="md-tool" title="Insert image" aria-label="Insert image"
                  :disabled="uploadingImage > 0" @click="imageInput?.click()">
            <span aria-hidden="true">🖼</span>
          </button>
        </div>

        <div class="dropzone" :class="{ over: dragOver }"
             @dragover="onDragOver" @dragleave="dragOver = false" @drop="onDrop">
          <textarea id="post-content" ref="contentEl" v-model="content" rows="18"
                    spellcheck="true" aria-describedby="post-content-hint"
                    @keydown="onEditorKeydown" @paste="onPaste"></textarea>
          <div v-if="dragOver" class="dropzone-hint" aria-hidden="true">Drop images to upload</div>
        </div>

        <div class="editor-foot">
          <p id="post-content-hint" class="muted">
            Markdown supported. Paste or drop images. <kbd>Ctrl</kbd>+<kbd>Enter</kbd> to
            {{ isEdit && !isDraftPost ? 'save' : 'publish' }}.
          </p>
          <p class="muted stats" aria-live="polite">
            <span v-if="uploadingImage">Uploading {{ uploadingImage }}… · </span>
            {{ wordCount }} words · {{ charCount }} chars · ~{{ estMinutes }} min
          </p>
        </div>
        <input ref="imageInput" type="file" accept="image/jpeg,image/png,image/webp" multiple
               style="display: none" @change="onImagePicked" />
      </div>

      <div v-show="isWide || mode === 'preview'" class="pane preview-pane">
        <div class="preview-head">
          <span class="preview-label">Preview</span>
          <span v-if="previewMinutes" class="muted">{{ previewMinutes }} min read</span>
          <span class="spacer"></span>
          <span v-if="previewBusy" class="muted preview-busy">rendering…</span>
        </div>
        <p v-if="previewError" class="muted preview-note" role="status">{{ previewError }}</p>
        <div v-if="previewHtml" ref="previewEl" class="post-body preview-body" v-html="previewHtml"></div>
        <p v-else class="muted preview-empty">Nothing to preview yet — start writing.</p>
      </div>
    </div>

    <label for="post-tags">Tags</label>
    <input id="post-tags" v-model="tagsInput" maxlength="200"
           placeholder="rust, databases, performance" aria-describedby="post-tags-hint" />
    <p id="post-tags-hint" class="muted" :class="{ error: parsedTags.length > MAX_TAGS }" style="margin-top: var(--sp-2);">
      Comma-separated, up to {{ MAX_TAGS }}. “C++” and “c++” are the same tag.
    </p>
    <ul v-if="parsedTags.length" class="tag-preview" aria-label="Tags preview">
      <li v-for="(t, i) in parsedTags" :key="i" class="tag-chip" :class="{ over: i >= MAX_TAGS }">#{{ t }}</li>
    </ul>

    <template v-if="isDraftPost">
      <label for="post-schedule" style="margin-top: var(--sp-5);">Schedule for later (optional)</label>
      <div class="row tight">
        <input id="post-schedule" type="datetime-local" v-model="scheduleAt" :min="minSchedule"
               :disabled="loading || savingDraft" />
        <button v-if="scheduleAt" type="button" class="btn quiet" @click="scheduleAt = ''">Clear</button>
      </div>
    </template>

    <p v-if="error" class="error" role="alert" style="margin-top: var(--sp-3);">{{ error }}</p>

    <div class="row tight editor-actions">
      <button :disabled="!canSave">{{ primaryLabel }}</button>
      <button type="button" class="ghost" :disabled="!canSave" @click="save(true)">{{ secondaryLabel }}</button>
      <router-link :to="cancelTarget" class="btn quiet">Cancel</router-link>
      <span v-if="dirty" class="muted unsaved" aria-live="polite">● Unsaved</span>
    </div>
  </form>
</template>

<style scoped>
.editor-tabs { margin: var(--sp-4) 0 var(--sp-3); }
.counter { margin: var(--sp-1) 0 0; font-size: 0.75rem; text-align: right; }

.recover-banner {
  display: flex; flex-wrap: wrap; gap: var(--sp-3);
  align-items: center; justify-content: space-between;
  padding: var(--sp-3) var(--sp-4); margin-bottom: var(--sp-4);
  border: 1px solid var(--accent); border-radius: var(--radius);
  background: color-mix(in srgb, var(--accent) 8%, transparent);
}
.schedule-note { margin: 0 0 var(--sp-3); }

.editor-panes.split {
  display: grid;
  grid-template-columns: repeat(2, minmax(0, 1fr));
  gap: var(--sp-4);
  align-items: start;
}
.pane { min-width: 0; }

.md-toolbar {
  display: flex; flex-wrap: wrap; gap: 2px; align-items: center;
  padding: var(--sp-1); margin-bottom: var(--sp-2);
  border: 1px solid var(--border); border-radius: var(--radius);
  background: var(--bg-inset);
  position: sticky; top: calc(var(--header-h, 3.5rem) + 4px); z-index: 2;
}
.md-tool {
  min-width: 2.1rem; height: 2.1rem; padding: 0 var(--sp-2);
  background: transparent; color: var(--text-dim); border: 0; border-radius: var(--radius-sm);
  font-size: 0.85rem; font-weight: 600; box-shadow: none;
}
.md-tool:hover:not(:disabled) { background: var(--bg-elev2); color: var(--text); }
.md-bold { font-weight: 800; }
.md-italic { font-style: italic; font-family: Georgia, serif; }
.md-strike { text-decoration: line-through; }
.md-code, .md-fence { font-family: var(--font-mono); font-size: 0.75rem; }
.md-sep { width: 1px; height: 1.4rem; background: var(--border); margin: 0 var(--sp-1); }

.dropzone { position: relative; }
.dropzone textarea { min-height: 18rem; resize: vertical; font-family: var(--font-mono); font-size: 0.92rem; line-height: 1.6; }
.dropzone.over textarea { outline: 2px dashed var(--accent); outline-offset: 2px; }
.dropzone-hint {
  position: absolute; inset: 0; display: grid; place-items: center;
  pointer-events: none; font-weight: 600; color: var(--accent);
  background: color-mix(in srgb, var(--bg) 70%, transparent); border-radius: var(--radius);
}

.editor-foot {
  display: flex; flex-wrap: wrap; justify-content: space-between; gap: var(--sp-2);
  margin-top: var(--sp-2); font-size: var(--step--1);
}
.editor-foot p { margin: 0; }
.stats { font-variant-numeric: tabular-nums; }
kbd {
  font-family: var(--font-mono); font-size: 0.72em; padding: 0 0.3em;
  border: 1px solid var(--border); border-bottom-width: 2px; border-radius: 4px;
}

.preview-pane { display: flex; flex-direction: column; min-height: 12rem; }
.preview-head {
  display: flex; align-items: center; gap: var(--sp-2);
  margin-bottom: var(--sp-2); font-size: var(--step--1);
  min-height: 2.6rem;
}
.preview-label { font-weight: 600; text-transform: uppercase; letter-spacing: 0.06em; color: var(--text-dim); }
.preview-busy { font-style: italic; }
.preview-note { margin: 0 0 var(--sp-2); }
.preview-body, .preview-empty {
  flex: 1; padding: var(--sp-4);
  background: var(--bg-inset); border: 1px solid var(--border); border-radius: var(--radius);
  max-height: 40rem; overflow-y: auto;
}
.preview-empty { margin: 0; display: flex; align-items: center; justify-content: center; }
.editor-panes:not(.split) .preview-body { max-height: none; }

.tag-preview { display: flex; flex-wrap: wrap; gap: var(--sp-1); list-style: none; padding: 0; margin: var(--sp-2) 0 0; }
.tag-chip {
  font-size: 0.78rem; padding: 0.15rem 0.55rem; border-radius: 999px;
  background: var(--bg-inset); border: 1px solid var(--border-soft); color: var(--text-dim);
}
.tag-chip.over { text-decoration: line-through; opacity: 0.6; }

.editor-actions { margin-top: var(--sp-5); flex-wrap: wrap; }
.unsaved { font-size: var(--step--1); margin-left: auto; }

@media (max-width: 600px) {
  .editor-actions > button, .editor-actions > .btn { flex: 1 1 100%; }
  .unsaved { margin-left: 0; }
}
</style>
