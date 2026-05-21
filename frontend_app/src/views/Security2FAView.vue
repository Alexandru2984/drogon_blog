<script setup lang="ts">
import { onMounted, ref } from 'vue'
import QRCode from 'qrcode'
import { twoFactorApi, webauthnRegister, type Passkey, type Status2fa }
  from '@/api/twofactor'
import { useToastStore } from '@/stores/toast'

const toasts = useToastStore()

const status      = ref<Status2fa | null>(null)
const passkeys    = ref<Passkey[]>([])
const setupSecret = ref('')
const setupUrl    = ref('')
const setupQrSrc  = ref('')
const setupCode   = ref('')
const newCodes    = ref<string[]>([])
const acceptCodes = ref(false)
const passkeyNick = ref('')
const disablePw   = ref('')
const disableCode = ref('')
const regenPw     = ref('')
const loading     = ref(false)
const error       = ref('')

async function refresh() {
  status.value   = await twoFactorApi.status()
  passkeys.value = (await twoFactorApi.webauthnList()).credentials
}
onMounted(refresh)

async function startTotp() {
  error.value = ''
  try {
    const r = await twoFactorApi.totpSetup()
    setupSecret.value = r.secret
    setupUrl.value    = r.otpauth_url
    setupQrSrc.value  = await QRCode.toDataURL(r.otpauth_url, { margin: 1, width: 220 })
  } catch (e: any) {
    error.value = e?.response?.data?.error ?? 'Setup failed'
  }
}

async function confirmTotp() {
  error.value = ''; loading.value = true
  try {
    const r = await twoFactorApi.totpConfirm(setupCode.value.trim())
    newCodes.value = r.recovery_codes
    setupSecret.value = ''
    setupCode.value   = ''
    await refresh()
  } catch (e: any) {
    error.value = e?.response?.data?.error ?? 'Invalid code'
  } finally { loading.value = false }
}

async function addPasskey() {
  error.value = ''; loading.value = true
  try {
    const r = await webauthnRegister(passkeyNick.value || 'Passkey')
    passkeyNick.value = ''
    if (Array.isArray(r.recovery_codes) && r.recovery_codes.length) {
      newCodes.value = r.recovery_codes
    }
    await refresh()
    toasts.push('Passkey added', 'ok')
  } catch (e: any) {
    error.value = e?.response?.data?.error ?? e?.message ?? 'Could not add passkey'
  } finally { loading.value = false }
}

async function removePasskey(id: number) {
  if (!confirm('Remove this passkey? You will need another method to sign in.')) return
  try {
    await twoFactorApi.webauthnRemove(id)
    await refresh()
  } catch (e: any) {
    toasts.push(e?.response?.data?.error ?? 'Could not remove passkey', 'error')
  }
}

async function regenerateCodes() {
  error.value = ''; loading.value = true
  try {
    const r = await twoFactorApi.regenerateRecoveryCodes(regenPw.value)
    newCodes.value = r.recovery_codes
    regenPw.value = ''
    await refresh()
  } catch (e: any) {
    error.value = e?.response?.data?.error ?? 'Could not regenerate codes'
  } finally { loading.value = false }
}

async function disableAll() {
  error.value = ''; loading.value = true
  try {
    await twoFactorApi.disable({ password: disablePw.value, totp_code: disableCode.value })
    disablePw.value = ''
    disableCode.value = ''
    await refresh()
    toasts.push('Two-factor authentication disabled', 'ok')
  } catch (e: any) {
    error.value = e?.response?.data?.error ?? 'Could not disable 2FA'
  } finally { loading.value = false }
}

function copyCodes() {
  navigator.clipboard.writeText(newCodes.value.join('\n'))
    .then(() => toasts.push('Recovery codes copied to clipboard', 'ok'))
}
</script>

<template>
  <div style="max-width: 720px; margin: 2rem auto;">
    <h2>Two-factor authentication</h2>
    <p class="muted">
      Strongly recommended. Without 2FA, anyone who learns your password owns your
      account. With it, they additionally need a code from your phone or a hardware key.
    </p>

    <div v-if="status" class="card" style="margin-top: 1rem;">
      <h3>Status</h3>
      <ul>
        <li>Authenticator app (TOTP): <strong>{{ status.totp_enabled ? 'enabled' : 'disabled' }}</strong></li>
        <li>Passkeys: <strong>{{ status.passkeys_count }}</strong></li>
        <li>Recovery codes remaining: <strong>{{ status.recovery_codes_left }}</strong></li>
      </ul>
    </div>

    <!-- ------- New recovery codes (one-time display) ------- -->
    <div v-if="newCodes.length" class="card" style="margin-top: 1rem; border-color: #d97706;">
      <h3>Save your recovery codes</h3>
      <p class="muted">
        Each code works <strong>once</strong> if you lose your authenticator. They will
        not be shown again. Print them, store them in a password manager, or write them on paper.
      </p>
      <pre style="background: #f8f8f8; padding: 0.75rem; border-radius: 6px;">{{ newCodes.join('\n') }}</pre>
      <div style="display: flex; gap: 0.5rem; margin-top: 0.5rem;">
        <button @click="copyCodes">Copy to clipboard</button>
        <label style="display: flex; align-items: center; gap: 0.4rem;">
          <input type="checkbox" v-model="acceptCodes" /> I have saved them
        </label>
      </div>
      <button :disabled="!acceptCodes" @click="newCodes = []" style="margin-top: 0.5rem;">
        Dismiss
      </button>
    </div>

    <!-- ------- TOTP setup ------- -->
    <div v-if="status && !status.totp_enabled" class="card" style="margin-top: 1rem;">
      <h3>Set up authenticator app</h3>
      <p class="muted">Works with Google Authenticator, 1Password, Authy, Bitwarden, …</p>
      <button v-if="!setupSecret" @click="startTotp">Begin setup</button>
      <div v-if="setupSecret">
        <p>Scan with your authenticator app, or enter the secret manually:</p>
        <p><img v-if="setupQrSrc" :src="setupQrSrc" alt="TOTP setup QR" /></p>
        <p><code>{{ setupSecret }}</code></p>
        <form @submit.prevent="confirmTotp" style="margin-top: 0.75rem;">
          <label for="totp-confirm">Enter the current 6-digit code to confirm</label>
          <input id="totp-confirm" v-model="setupCode" inputmode="numeric" pattern="[0-9]{6}" maxlength="6" required />
          <button :disabled="loading" style="margin-top: 0.5rem;">{{ loading ? 'Confirming…' : 'Confirm' }}</button>
        </form>
      </div>
    </div>

    <!-- ------- Passkey management ------- -->
    <div class="card" style="margin-top: 1rem;">
      <h3>Passkeys</h3>
      <p class="muted">Hardware security keys, Touch ID, Windows Hello, phone passkeys.</p>
      <form @submit.prevent="addPasskey" style="display: flex; gap: 0.5rem; align-items: flex-end;">
        <div style="flex: 1;">
          <label for="passkey-nick">Nickname (optional)</label>
          <input id="passkey-nick" v-model="passkeyNick" placeholder="MacBook Touch ID" />
        </div>
        <button :disabled="loading">Add passkey</button>
      </form>
      <ul v-if="passkeys.length" style="margin-top: 1rem;">
        <li v-for="p in passkeys" :key="p.id"
            style="display: flex; gap: 1rem; align-items: center;">
          <span style="flex: 1;">
            <strong>{{ p.nickname || 'Unnamed passkey' }}</strong>
            <small class="muted">— added {{ new Date(p.created_at).toLocaleDateString() }}</small>
          </span>
          <button @click="removePasskey(p.id)" class="danger">Remove</button>
        </li>
      </ul>
    </div>

    <!-- ------- Recovery codes regen ------- -->
    <div v-if="status && (status.totp_enabled || status.passkeys_count > 0)" class="card" style="margin-top: 1rem;">
      <h3>Recovery codes</h3>
      <p class="muted">Regenerating invalidates any previous codes.</p>
      <form @submit.prevent="regenerateCodes" style="display: flex; gap: 0.5rem; align-items: flex-end;">
        <div style="flex: 1;">
          <label>Current password</label>
          <input v-model="regenPw" type="password" autocomplete="current-password" required />
        </div>
        <button :disabled="loading">Generate new codes</button>
      </form>
    </div>

    <!-- ------- Disable ------- -->
    <div v-if="status && (status.totp_enabled || status.passkeys_count > 0)" class="card" style="margin-top: 1rem; border-color: #dc2626;">
      <h3>Disable all 2FA factors</h3>
      <p class="muted">
        Removes TOTP, every passkey, and the recovery codes. Requires your password
        and a current authenticator code so a hijacked session alone cannot do this.
      </p>
      <form @submit.prevent="disableAll">
        <label>Current password</label>
        <input v-model="disablePw" type="password" autocomplete="current-password" required />
        <label>Current TOTP code</label>
        <input v-model="disableCode" inputmode="numeric" pattern="[0-9]{6}" maxlength="6" required />
        <button :disabled="loading" class="danger" style="margin-top: 0.5rem;">Disable 2FA</button>
      </form>
    </div>

    <p v-if="error" class="error" style="margin-top: 1rem;">{{ error }}</p>
  </div>
</template>

<style scoped>
.danger { background: #dc2626; color: white; }
</style>
