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
  <div class="card" style="max-width: 460px; margin: 2rem auto;">
    <h2>Two-factor verification</h2>
    <p class="muted">Pick a verification method enrolled on your account.</p>

    <div class="tabs" style="display: flex; gap: 0.5rem; margin: 1rem 0;">
      <button v-if="methods.includes('totp')"     :class="{ active: tab==='totp' }"     @click="tab='totp'">Authenticator app</button>
      <button v-if="methods.includes('webauthn')" :class="{ active: tab==='webauthn' }" @click="tab='webauthn'">Passkey / security key</button>
      <button v-if="methods.includes('recovery')" :class="{ active: tab==='recovery' }" @click="tab='recovery'">Recovery code</button>
    </div>

    <form v-if="tab==='totp'" @submit.prevent="submitTotp">
      <label for="totp-code">6-digit code</label>
      <input id="totp-code" v-model="totpCode" inputmode="numeric" pattern="[0-9]{6}" maxlength="6" autofocus autocomplete="one-time-code" required />
      <p v-if="error" class="error" style="margin-top: 0.75rem;">{{ error }}</p>
      <button :disabled="loading" style="margin-top: 1rem; width: 100%;">
        {{ loading ? 'Verifying…' : 'Verify' }}
      </button>
    </form>

    <div v-if="tab==='webauthn'">
      <p class="muted">Use your security key or platform authenticator (Touch ID, Windows Hello, …).</p>
      <p v-if="error" class="error">{{ error }}</p>
      <button :disabled="loading" @click="submitPasskey" style="width: 100%;">
        {{ loading ? 'Waiting on authenticator…' : 'Authenticate with passkey' }}
      </button>
    </div>

    <form v-if="tab==='recovery'" @submit.prevent="submitRecovery">
      <label for="recov-code">Recovery code (XXXX-XXXX)</label>
      <input id="recov-code" v-model="recoveryCode" placeholder="ABCD-EFGH" autofocus required />
      <p v-if="error" class="error" style="margin-top: 0.75rem;">{{ error }}</p>
      <button :disabled="loading" style="margin-top: 1rem; width: 100%;">
        {{ loading ? 'Verifying…' : 'Sign in' }}
      </button>
    </form>
  </div>
</template>

<style scoped>
.tabs button {
  flex: 1; padding: 0.45rem 0.6rem; background: transparent;
  border: 1px solid var(--border, #ddd); border-radius: 6px;
  font-size: 0.9rem; cursor: pointer;
}
.tabs button.active { background: var(--accent, #2563eb); color: white; border-color: transparent; }
</style>
