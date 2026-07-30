<script setup lang="ts">
import { ref, computed } from 'vue'
import { useRouter } from 'vue-router'
import { accountApi } from '@/api/account'
import { useAuthStore } from '@/stores/auth'
import { useToastStore } from '@/stores/toast'
import { useMessagesStore } from '@/stores/messages'

// The two things a person is entitled to do with an account holding their
// data: take a copy of it, and end it. Both are on one page, deliberately —
// "download everything first" is the advice anyone would give before the
// second one, and it is worth putting the two next to each other.

const auth     = useAuthStore()
const router   = useRouter()
const toasts   = useToastStore()
const messages = useMessagesStore()

const username = computed(() => auth.user?.username ?? '')

// ---- Export -------------------------------------------------------------

const exportPw    = ref('')
const exportBusy  = ref(false)
const exportError = ref('')

async function doExport() {
  if (!exportPw.value || exportBusy.value) return
  exportBusy.value = true
  exportError.value = ''
  try {
    const { blob, filename } = await accountApi.exportData(exportPw.value)
    // Object URL rather than a data: URL — a data URL of a multi-megabyte
    // export hits the URL-length ceiling in several browsers, and the CSP
    // would have to allow data: in a place it currently does not.
    const url = URL.createObjectURL(blob)
    const a = document.createElement('a')
    a.href = url
    a.download = filename
    document.body.appendChild(a)
    a.click()
    a.remove()
    // Revoked on the next frame, not immediately: Safari has not started
    // reading the blob by the time click() returns.
    requestAnimationFrame(() => URL.revokeObjectURL(url))
    exportPw.value = ''
    toasts.push('Export downloaded', 'ok')
  } catch (e: any) {
    exportError.value = e?.response?.data?.error ?? 'Could not build the export'
  } finally {
    exportBusy.value = false
  }
}

// ---- Deletion -----------------------------------------------------------

const deletePw      = ref('')
const deleteConfirm = ref('')
const deleteBusy    = ref(false)
const deleteError   = ref('')
const deleteArmed   = ref(false)

// Both fields, and the typed name has to match exactly. The password is the
// security control; the typed username is what stops the button being
// pressed by someone who was reading something else.
const canDelete = computed(() =>
  !!deletePw.value && deleteConfirm.value === username.value && !deleteBusy.value)

async function doDelete() {
  if (!canDelete.value) return
  deleteBusy.value = true
  deleteError.value = ''
  try {
    await accountApi.deleteAccount(deletePw.value, deleteConfirm.value)
    // The server has already revoked every session, so the socket is
    // talking to something that no longer authorises it.
    messages.disconnectSocket()
    messages.clear()
    auth.clearSession()
    toasts.push('Your account has been deleted', 'ok')
    router.push({ name: 'home' })
  } catch (e: any) {
    deleteError.value = e?.response?.data?.error ?? 'Could not delete the account'
  } finally {
    deleteBusy.value = false
  }
}
</script>

<template>
  <h1 class="page-title">Your data</h1>

  <section class="card" aria-labelledby="export-heading">
    <h2 id="export-heading" class="section-title">Download a copy</h2>
    <p class="muted section-lead">
      Everything on the account in one JSON file: your profile, posts and
      drafts, comments, likes, saved posts, follows in both directions, every
      message you sent or received, your notifications and your sign-in
      history.
    </p>

    <form @submit.prevent="doExport">
      <label for="export-password">Confirm your password</label>
      <input
        id="export-password"
        v-model="exportPw"
        type="password"
        autocomplete="current-password"
        aria-describedby="export-password-hint"
      />
      <p id="export-password-hint" class="muted" style="margin-top: var(--sp-2);">
        Asked for because a signed-in browser is not proof that you are the
        one holding it.
      </p>

      <p v-if="exportError" class="error" role="alert" style="margin-top: var(--sp-3);">
        {{ exportError }}
      </p>

      <div class="row tight" style="margin-top: var(--sp-4);">
        <button :disabled="!exportPw || exportBusy">
          {{ exportBusy ? 'Preparing…' : 'Download my data' }}
        </button>
      </div>
    </form>
  </section>

  <section class="card destructive" aria-labelledby="delete-heading">
    <h2 id="delete-heading" class="section-title">Delete this account</h2>
    <p class="section-lead">
      This removes your posts and drafts, your comments, your likes and saved
      posts, your follows, every message you sent or received, and your
      profile. Your name and address are released, so the username becomes
      available again.
    </p>
    <p class="muted section-lead">
      One exception, and it is deliberate: a comment of yours that other
      people replied to stays in place with its text replaced, because
      deleting it would delete their replies too. Nothing identifying you
      remains on it.
    </p>
    <p class="muted section-lead">
      <strong>This cannot be undone.</strong> Download a copy of your data
      first if you might want it.
    </p>

    <!-- The form is behind a deliberate step. A password field and a
         Delete button sitting in the open, one tab away from everything
         else on the page, is a mis-click waiting to happen. -->
    <div v-if="!deleteArmed" class="row tight">
      <button type="button" class="ghost danger-text" @click="deleteArmed = true">
        I want to delete my account
      </button>
    </div>

    <form v-else @submit.prevent="doDelete">
      <label for="delete-password">Your password</label>
      <input
        id="delete-password"
        v-model="deletePw"
        type="password"
        autocomplete="current-password"
      />

      <label for="delete-confirm">
        Type <code>{{ username }}</code> to confirm
      </label>
      <input
        id="delete-confirm"
        v-model="deleteConfirm"
        autocomplete="off"
        spellcheck="false"
        :aria-invalid="!!deleteConfirm && deleteConfirm !== username"
      />

      <p v-if="deleteError" class="error" role="alert" style="margin-top: var(--sp-3);">
        {{ deleteError }}
      </p>

      <div class="row tight" style="margin-top: var(--sp-4);">
        <button class="danger" :disabled="!canDelete">
          {{ deleteBusy ? 'Deleting…' : 'Delete my account permanently' }}
        </button>
        <button type="button" class="quiet" :disabled="deleteBusy"
                @click="deleteArmed = false; deletePw = ''; deleteConfirm = ''">
          Cancel
        </button>
      </div>
    </form>
  </section>
</template>

<style scoped>
.section-title {
  font-size: var(--step-1);
  margin: 0 0 var(--sp-2);
}
.section-lead { margin: 0 0 var(--sp-4); }

.card + .card { margin-top: var(--sp-5); }

/* The username the reader has to copy, set apart from the sentence around
   it so a trailing space or a capital is visible. */
code {
  background: var(--bg-inset);
  padding: 0.1em 0.4em;
  border-radius: var(--radius-sm);
  font-family: var(--font-mono);
  font-size: 0.92em;
}
</style>
