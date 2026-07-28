<script setup lang="ts">
import { onMounted, ref } from 'vue'
import QRCode from 'qrcode'
import { twoFactorApi, webauthnRegister, type Passkey, type Status2fa }
  from '@/api/twofactor'
import { accountApi, type SessionEntry } from '@/api/account'
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

// ---- Password + active sessions ----
const sessions      = ref<SessionEntry[]>([])
const currentPw     = ref('')
const newPw         = ref('')
const confirmPw     = ref('')
const pwBusy        = ref(false)
const sessionsBusy  = ref(false)

async function refresh() {
  status.value   = await twoFactorApi.status()
  passkeys.value = (await twoFactorApi.webauthnList()).credentials
  await refreshSessions()
}
onMounted(refresh)

async function refreshSessions() {
  try {
    sessions.value = await accountApi.listSessions()
  } catch {
    // Non-fatal: the rest of the page is still useful without the list.
    sessions.value = []
  }
}

async function submitPasswordChange() {
  if (newPw.value !== confirmPw.value) {
    toasts.push('New passwords do not match', 'error')
    return
  }
  pwBusy.value = true
  try {
    const r = await accountApi.changePassword(currentPw.value, newPw.value)
    currentPw.value = newPw.value = confirmPw.value = ''
    // Say how many sessions were cut: a user changing their password
    // because they suspect someone else is in the account wants to see
    // that something actually happened.
    toasts.push(
      r.revoked_sessions > 0
        ? `Password changed — ${r.revoked_sessions} other session(s) signed out`
        : 'Password changed',
      'ok',
    )
    await refreshSessions()
  } catch (e: any) {
    toasts.push(e?.response?.data?.error ?? 'Could not change password', 'error')
  } finally {
    pwBusy.value = false
  }
}

async function revokeOne(sid: string) {
  sessionsBusy.value = true
  try {
    await accountApi.revokeSession(sid)
    toasts.push('Session signed out', 'ok')
    await refreshSessions()
  } catch (e: any) {
    toasts.push(e?.response?.data?.error ?? 'Could not sign out that session', 'error')
  } finally {
    sessionsBusy.value = false
  }
}

async function revokeOthers() {
  sessionsBusy.value = true
  try {
    const r = await accountApi.revokeOtherSessions()
    toasts.push(`${r.revoked_sessions} session(s) signed out`, 'ok')
    await refreshSessions()
  } catch (e: any) {
    toasts.push(e?.response?.data?.error ?? 'Could not sign out other sessions', 'error')
  } finally {
    sessionsBusy.value = false
  }
}

// Turn a raw User-Agent into something a person can recognise. Deliberately
// crude: the goal is "is this me?", not analytics. Order matters — Edge and
// Chrome both contain "Chrome", Safari contains neither.
function describeAgent(ua: string): string {
  if (!ua) return 'Unknown device'
  const browser =
    /Edg\//.test(ua)                        ? 'Edge'    :
    /OPR\/|Opera/.test(ua)                  ? 'Opera'   :
    /Firefox\//.test(ua)                    ? 'Firefox' :
    /Chrome\//.test(ua)                     ? 'Chrome'  :
    /Safari\//.test(ua)                     ? 'Safari'  :
    /curl\//.test(ua)                       ? 'curl'    : 'Browser'
  const os =
    /Android/.test(ua)                      ? 'Android' :
    /iPhone|iPad|iPod/.test(ua)             ? 'iOS'     :
    /Mac OS X|Macintosh/.test(ua)           ? 'macOS'   :
    /Windows/.test(ua)                      ? 'Windows' :
    /Linux/.test(ua)                        ? 'Linux'   : ''
  return os ? `${browser} on ${os}` : browser
}

function formatWhen(ts: string): string {
  // Postgres hands back "2026-07-28 17:22:42.970854" with no zone marker;
  // it is UTC, so say so before Date parses it as local time.
  const d = new Date(ts.replace(' ', 'T') + 'Z')
  return isNaN(d.getTime()) ? ts : d.toLocaleString()
}

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
    <h2>Account security</h2>

    <!-- ------- Change password ------- -->
    <div class="card" style="margin-top: 1rem;">
      <h3>Change password</h3>
      <p class="muted">
        Changing your password signs out every other device. The one you are
        using now stays signed in.
      </p>
      <form @submit.prevent="submitPasswordChange">
        <label for="cur-pw">Current password</label>
        <input id="cur-pw" v-model="currentPw" type="password"
               autocomplete="current-password" required />

        <label for="new-pw">New password</label>
        <input id="new-pw" v-model="newPw" type="password" minlength="8"
               autocomplete="new-password" required />

        <label for="conf-pw">Confirm new password</label>
        <input id="conf-pw" v-model="confirmPw" type="password" minlength="8"
               autocomplete="new-password" required />

        <button type="submit" :disabled="pwBusy" style="margin-top: 0.75rem;">
          {{ pwBusy ? 'Changing…' : 'Change password' }}
        </button>
      </form>
    </div>

    <!-- ------- Active sessions ------- -->
    <div class="card" style="margin-top: 1rem;">
      <h3>Where you are signed in</h3>
      <p class="muted">
        Every device currently holding a session. Sign out anything you do not
        recognise. Sessions also end whenever the server restarts.
      </p>

      <p v-if="!sessions.length" class="muted">No active sessions recorded.</p>

      <ul v-else style="list-style: none; padding: 0; margin: 0;">
        <li v-for="s in sessions" :key="s.sid"
            style="display: flex; gap: 0.75rem; align-items: center;
                   padding: 0.6rem 0; border-top: 1px solid var(--border);">
          <div style="flex: 1; min-width: 0;">
            <div>
              <strong>{{ describeAgent(s.user_agent) }}</strong>
              <span v-if="s.current" class="ok" style="margin-left: 0.4rem;">
                — this device
              </span>
            </div>
            <div class="muted" style="word-break: break-word;">
              {{ s.ip || 'unknown address' }} · last active {{ formatWhen(s.last_seen_at) }}
            </div>
          </div>
          <button v-if="!s.current" class="ghost" :disabled="sessionsBusy"
                  @click="revokeOne(s.sid)">
            Sign out
          </button>
        </li>
      </ul>

      <button v-if="sessions.length > 1" class="danger"
              :disabled="sessionsBusy" style="margin-top: 0.75rem;"
              @click="revokeOthers">
        Sign out everywhere else
      </button>
    </div>

    <h2 style="margin-top: 2rem;">Two-factor authentication</h2>
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
