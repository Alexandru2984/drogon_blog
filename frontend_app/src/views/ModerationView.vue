<script setup lang="ts">
import { ref, onMounted } from 'vue'
import { moderationApi, type Report, type ReportStatus } from '@/api/moderation'
import { useToastStore } from '@/stores/toast'

const toasts = useToastStore()

const status    = ref<ReportStatus>('open')
const reports   = ref<Report[]>([])
const loading   = ref(true)
// null: never loaded. false: loaded fine. true: the backend answered 404,
// i.e. this account is not staff — same response an ordinary user probing
// the URL would get (see helpers/Roles.h on why it's 404, not 403).
const forbidden = ref(false)
const error     = ref('')

// Which open report currently has its action form expanded, plus the
// draft values for it. One at a time — a moderator acting on a queue works
// row by row, and sharing a single set of fields keeps the template simple.
const actingOn  = ref<number | null>(null)
const busyId    = ref<number | null>(null)
const banDays   = ref(7)
const actionNote = ref('')

const tabs: { value: ReportStatus; label: string }[] = [
  { value: 'open',      label: 'Open' },
  { value: 'actioned',  label: 'Actioned' },
  { value: 'dismissed', label: 'Dismissed' },
]

async function load() {
  loading.value = true
  error.value = ''
  forbidden.value = false
  actingOn.value = null
  try {
    reports.value = await moderationApi.listReports(status.value)
  } catch (e: any) {
    if (e?.response?.status === 404) forbidden.value = true
    else error.value = e?.response?.data?.error ?? 'Could not load reports'
  } finally {
    loading.value = false
  }
}
onMounted(load)

function switchTab(s: ReportStatus) {
  status.value = s
  load()
}

function startAction(r: Report) {
  actingOn.value = r.id
  banDays.value = 7
  actionNote.value = ''
}

// Drop a resolved report from the local list instead of reloading the
// whole queue — the point of working a queue is watching it get shorter.
function drop(id: number) {
  reports.value = reports.value.filter(r => r.id !== id)
  actingOn.value = null
}

async function dismiss(r: Report) {
  busyId.value = r.id
  try {
    await moderationApi.resolveReport(r.id, 'dismissed')
    drop(r.id)
    toasts.push('Report dismissed', 'ok')
  } catch (e: any) {
    toasts.push(e?.response?.data?.error ?? 'Could not dismiss report', 'error')
  } finally {
    busyId.value = null
  }
}

// Hides the reported post/comment (or bans the reported user), then marks
// the report actioned. The two calls are sequential and not transactional
// across the HTTP boundary; if the resolve step fails after a successful
// hide/ban, the content stays hidden (the safe side to fail on) and the
// report stays open for a retry.
async function takeAction(r: Report) {
  busyId.value = r.id
  try {
    if (r.target_type === 'post') await moderationApi.hidePost(r.target_id, actionNote.value || undefined)
    else if (r.target_type === 'comment') await moderationApi.hideComment(r.target_id, actionNote.value || undefined)
    else await moderationApi.banUser(r.target_id, banDays.value, actionNote.value || undefined)

    await moderationApi.resolveReport(r.id, 'actioned', actionNote.value || undefined)
    drop(r.id)
    toasts.push('Action taken', 'ok')
  } catch (e: any) {
    toasts.push(e?.response?.data?.error ?? 'Could not complete action', 'error')
  } finally {
    busyId.value = null
  }
}

function when(s: string) {
  return s ? new Date(s.replace(' ', 'T') + 'Z').toLocaleString() : ''
}

// --- Manual overrides -----------------------------------------------------
// Covers the rest of the API surface (undoing a hide/ban outside the report
// flow that produced it) without tracking per-row hidden/banned state, which
// listReports does not return. A raw-id form is a reasonable shape for a
// staff-only tool used rarely.
const manual = ref<{ kind: 'unhide-post' | 'unhide-comment' | 'unban'; id: string }>({
  kind: 'unhide-post', id: '',
})
const manualBusy = ref(false)

async function runManual() {
  const id = Number(manual.value.id)
  if (!Number.isInteger(id) || id <= 0) {
    toasts.push('Enter a valid id', 'error')
    return
  }
  manualBusy.value = true
  try {
    if (manual.value.kind === 'unhide-post') await moderationApi.unhidePost(id)
    else if (manual.value.kind === 'unhide-comment') await moderationApi.unhideComment(id)
    else await moderationApi.unbanUser(id)
    toasts.push('Done', 'ok')
    manual.value.id = ''
  } catch (e: any) {
    toasts.push(e?.response?.data?.error ?? 'Action failed', 'error')
  } finally {
    manualBusy.value = false
  }
}
</script>

<template>
  <h1 class="page-title">Moderation</h1>

  <div v-if="forbidden" class="empty-state">
    <span class="emoji" aria-hidden="true">🔒</span>
    <p>This page is for moderators and admins only.</p>
    <router-link to="/" class="btn ghost">Back to the feed</router-link>
  </div>

  <template v-else>
    <div class="row tight mod-tabs">
      <button
        v-for="tab in tabs"
        :key="tab.value"
        :aria-pressed="status === tab.value"
        :class="['sm', status === tab.value ? '' : 'ghost']"
        @click="switchTab(tab.value)"
      >{{ tab.label }}</button>
    </div>

    <template v-if="loading">
      <p class="visually-hidden" role="status">Loading reports…</p>
      <div v-for="n in 3" :key="n" class="card" aria-hidden="true">
        <div class="skeleton line" style="height: 1.2em; width: 40%;"></div>
        <div class="skeleton line medium"></div>
      </div>
    </template>

    <div v-else-if="error" class="empty-state" role="alert">
      <span class="emoji" aria-hidden="true">⚠️</span>
      <p class="error">{{ error }}</p>
    </div>

    <div v-else-if="!reports.length" class="empty-state">
      <span class="emoji" aria-hidden="true">✅</span>
      <p>Nothing here.</p>
    </div>

    <article v-for="r in reports" :key="r.id" class="card report-row">
      <header class="row tight">
        <span class="badge accent">{{ r.target_type }} #{{ r.target_id }}</span>
        <span class="badge">{{ r.reason }}</span>
        <span class="spacer"></span>
        <time class="muted" style="font-size: var(--step--1);">{{ when(r.created_at) }}</time>
      </header>

      <p v-if="r.detail" class="report-detail">{{ r.detail }}</p>
      <p class="muted" style="font-size: var(--step--1); margin: 0 0 var(--sp-2);">
        Reported by {{ r.reporter || 'a deleted account' }}
      </p>

      <p class="row tight" style="margin: var(--sp-2) 0;">
        <router-link v-if="r.target_type === 'user'" :to="{ name: 'profile', params: { id: r.target_id } }">
          View profile
        </router-link>
        <router-link v-else-if="r.target_type === 'post'" :to="{ name: 'post', params: { id: r.target_id } }">
          View post
        </router-link>
        <router-link v-else-if="r.target_type === 'comment' && r.post_id" :to="{ name: 'post', params: { id: r.post_id } }">
          View the post it's on
        </router-link>
      </p>

      <template v-if="status === 'open'">
        <div v-if="actingOn !== r.id" class="row tight">
          <button class="sm" :disabled="busyId === r.id" @click="startAction(r)">
            Take action
          </button>
          <button class="ghost sm" :disabled="busyId === r.id" @click="dismiss(r)">
            {{ busyId === r.id ? 'Working…' : 'Dismiss' }}
          </button>
        </div>

        <div v-else class="action-form">
          <p class="muted" style="font-size: var(--step--1);">
            {{ r.target_type === 'user'
              ? 'Suspends the account and signs it out everywhere.'
              : `Hides the ${r.target_type} from every reader immediately.` }}
          </p>
          <label v-if="r.target_type === 'user'" :for="`ban-days-${r.id}`">Suspend for (days)</label>
          <input
            v-if="r.target_type === 'user'"
            :id="`ban-days-${r.id}`"
            v-model.number="banDays"
            type="number" min="1" max="3650"
          />
          <label :for="`action-note-${r.id}`">Note (kept with the moderation record)</label>
          <textarea :id="`action-note-${r.id}`" v-model="actionNote" rows="2" maxlength="2048"></textarea>
          <div class="row tight" style="margin-top: var(--sp-2);">
            <button class="sm danger" type="button" :disabled="busyId === r.id" @click="takeAction(r)">
              {{ busyId === r.id ? 'Working…' : (r.target_type === 'user' ? 'Suspend account' : 'Hide content') }}
            </button>
            <button type="button" class="quiet sm" @click="actingOn = null">Cancel</button>
          </div>
        </div>
      </template>
    </article>

    <details class="card mod-manual">
      <summary>Manual override (unhide / unban by id)</summary>
      <p class="muted" style="font-size: var(--step--1);">
        For undoing an action taken outside a report, or one filed here earlier.
      </p>
      <div class="row tight">
        <select v-model="manual.kind">
          <option value="unhide-post">Unhide post</option>
          <option value="unhide-comment">Unhide comment</option>
          <option value="unban">Unban user</option>
        </select>
        <input v-model="manual.id" type="number" min="1" placeholder="id" style="max-width: 8rem;" />
        <button class="sm" :disabled="manualBusy" @click="runManual">
          {{ manualBusy ? 'Working…' : 'Apply' }}
        </button>
      </div>
    </details>
  </template>
</template>

<style scoped>
.mod-tabs { margin-bottom: var(--sp-4); }
.report-row { display: flex; flex-direction: column; gap: var(--sp-1); }
.report-detail { white-space: pre-wrap; overflow-wrap: anywhere; }
.action-form { margin-top: var(--sp-2); padding-top: var(--sp-2); border-top: 1px solid var(--border); }
.action-form label { font-size: var(--step--1); }
.mod-manual { margin-top: var(--sp-5); }
.mod-manual summary { cursor: pointer; font-weight: 600; }
.mod-manual .row { margin-top: var(--sp-3); }
</style>
