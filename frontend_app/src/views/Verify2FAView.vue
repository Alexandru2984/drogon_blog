<script setup lang="ts">
import { computed, onMounted, ref } from 'vue'
import { useRoute, useRouter } from 'vue-router'
import { twoFactorApi, webauthnAuthenticate } from '@/api/twofactor'
import { useAuthStore } from '@/stores/auth'
import { useToastStore } from '@/stores/toast'

const auth = useAuthStore()
const router = useRouter()
const route = useRoute()
const toasts = useToastStore()

// If the user lands here without a pending login (refresh, deep-link,
// direct nav), kick them back to /login.
onMounted(() => {
  if (!auth.needs2fa) router.replace('/login')
})

type Tab = 'totp' | 'webauthn' | 'recovery'
const methods = computed(() => auth.pending2fa?.methods ?? [])
const tab = ref<Tab>(
  (methods.value.includes('totp') && 'totp') ||
  (methods.value.includes('webauthn') && 'webauthn') ||
  'recovery'
)

const totpCode      = ref('')
const recoveryCode  = ref('')
const loading       = ref(false)
const error         = ref('')

function safeNext(raw: unknown): string {
  if (typeof raw !== 'string' || raw.length === 0) return '/'
  if (raw[0] !== '/' || raw.startsWith('//') || raw.startsWith('/\\')) return '/'
  return raw
}

async function submitTotp() {
  error.value = ''; loading.value = true
  try {
    const res = await twoFactorApi.verifyTotp(totpCode.value.trim())
    auth.finalizeAfter2fa(res.user)
    toasts.push(`Welcome back, ${res.user.username}`, 'ok')
    router.push(safeNext(route.query.next))
  } catch (e: any) {
    error.value = e?.response?.data?.error ?? 'Verification failed'
  } finally { loading.value = false }
}

async function submitRecovery() {
  error.value = ''; loading.value = true
  try {
    const res = await twoFactorApi.verifyRecovery(recoveryCode.value.trim())
    auth.finalizeAfter2fa(res.user)
    toasts.push('Signed in with recovery code. Generate a fresh set under Profile.', 'ok')
    router.push(safeNext(route.query.next))
  } catch (e: any) {
    error.value = e?.response?.data?.error ?? 'Invalid recovery code'
  } finally { loading.value = false }
}

async function submitPasskey() {
  error.value = ''; loading.value = true
  try {
    const res = await webauthnAuthenticate()
    auth.finalizeAfter2fa(res.user)
    toasts.push(`Welcome back, ${res.user.username}`, 'ok')
    router.push(safeNext(route.query.next))
  } catch (e: any) {
    error.value = e?.response?.data?.error ?? e?.message ?? 'Passkey verification failed'
  } finally { loading.value = false }
}
</script>

<template>
  <div class="card auth-card wide">
    <h1 class="auth-title">Two-factor verification</h1>
    <p class="muted">Pick a verification method enrolled on your account.</p>

    <!-- role=tablist rather than three loose buttons: without it a screen
         reader announces them as unrelated controls and never says which
         one is selected. The .tabs styling wraps to one row each when the
         labels cannot fit side by side. -->
    <div class="tabs" role="tablist" aria-label="Verification method">
      <button v-if="methods.includes('totp')" role="tab" type="button"
              :aria-selected="tab==='totp'" :class="{ active: tab==='totp' }"
              @click="tab='totp'">Authenticator app</button>
      <button v-if="methods.includes('webauthn')" role="tab" type="button"
              :aria-selected="tab==='webauthn'" :class="{ active: tab==='webauthn' }"
              @click="tab='webauthn'">Passkey</button>
      <button v-if="methods.includes('recovery')" role="tab" type="button"
              :aria-selected="tab==='recovery'" :class="{ active: tab==='recovery' }"
              @click="tab='recovery'">Recovery code</button>
    </div>

    <form v-if="tab==='totp'" @submit.prevent="submitTotp">
      <label for="totp-code">6-digit code</label>
      <input id="totp-code" v-model="totpCode" inputmode="numeric" pattern="[0-9]{6}"
             maxlength="6" autofocus autocomplete="one-time-code" required class="code-input" />
      <p v-if="error" class="error" role="alert" style="margin-top: var(--sp-3);">{{ error }}</p>
      <button :disabled="loading" style="margin-top: var(--sp-4); width: 100%;">
        {{ loading ? 'Verifying…' : 'Verify' }}
      </button>
    </form>

    <div v-if="tab==='webauthn'">
      <p class="muted">Use your security key or platform authenticator (Touch ID, Windows Hello, …).</p>
      <p v-if="error" class="error" role="alert">{{ error }}</p>
      <button :disabled="loading" @click="submitPasskey" style="margin-top: var(--sp-4); width: 100%;">
        {{ loading ? 'Waiting on authenticator…' : 'Authenticate with passkey' }}
      </button>
    </div>

    <form v-if="tab==='recovery'" @submit.prevent="submitRecovery">
      <label for="recov-code">Recovery code (XXXX-XXXX)</label>
      <input id="recov-code" v-model="recoveryCode" placeholder="ABCD-EFGH"
             autocomplete="one-time-code" autocapitalize="characters" spellcheck="false"
             autofocus required class="code-input" />
      <p v-if="error" class="error" role="alert" style="margin-top: var(--sp-3);">{{ error }}</p>
      <button :disabled="loading" style="margin-top: var(--sp-4); width: 100%;">
        {{ loading ? 'Verifying…' : 'Sign in' }}
      </button>
    </form>
  </div>
</template>

<style scoped>
/* One-time codes are read off a screen and typed character by character;
   proportional text makes transposition errors easy to miss. */
.code-input {
  font-family: var(--font-mono);
  letter-spacing: 0.12em;
}
</style>
