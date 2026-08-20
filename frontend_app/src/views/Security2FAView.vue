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
const managementPw = ref('')
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

const pageLoading = ref(true)

// allSettled, in parallel, deliberately.
//
// Sequentially awaiting the three calls meant a failure in the first one
// (2FA status) aborted the whole function, so the page rendered with no
// status card, no passkey list AND no session list — three unrelated
// features taken out by one error, with nothing on screen to say why. It
// also cost three serial round-trips on a page that needs one.
async function refresh() {
  const [s, pk, ses] = await Promise.allSettled([
    twoFactorApi.status(),
    twoFactorApi.webauthnList(),
    accountApi.listSessions(),
  ])
  if (s.status  === 'fulfilled') status.value   = s.value
  if (pk.status === 'fulfilled') passkeys.value = pk.value.credentials
  sessions.value = ses.status === 'fulfilled' ? ses.value : []
  if (s.status === 'rejected') {
    error.value = 'Could not load two-factor status. Reload to try again.'
  }
  pageLoading.value = false
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
  if (!managementPw.value) {
    error.value = 'Confirm your current password first'
    return
  }
  try {
    const r = await twoFactorApi.totpSetup(managementPw.value)
    managementPw.value = ''
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
  if (!managementPw.value) {
    error.value = 'Confirm your current password first'
    loading.value = false
    return
  }
  try {
    const r = await webauthnRegister(
      passkeyNick.value || 'Passkey', managementPw.value)
    passkeyNick.value = ''
    managementPw.value = ''
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
  if (!managementPw.value) {
    toasts.push('Confirm your current password first', 'error')
    return
  }
  if (!confirm('Remove this passkey? You will need another method to sign in.')) return
  try {
    await twoFactorApi.webauthnRemove(id, managementPw.value)
    managementPw.value = ''
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
  <div class="security-page">
    <h1 class="page-title">Account security</h1>

    <!-- ------- Change password ------- -->
    <div class="card stack-card">
      <h2 class="card-title">Change password</h2>
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

        <button type="submit" :disabled="pwBusy" style="margin-top: var(--sp-4);">
          {{ pwBusy ? 'Changing…' : 'Change password' }}
        </button>
      </form>
    </div>

    <!-- ------- Active sessions ------- -->
    <div class="card stack-card">
      <h2 class="card-title">Where you are signed in</h2>
      <p class="muted">
        Every device currently holding a session. Sign out anything you do not
        recognise. Sessions also end whenever the server restarts.
      </p>

      <div v-if="pageLoading" aria-hidden="true">
        <div v-for="n in 2" :key="n" class="session-row">
          <div style="flex: 1;">
            <div class="skeleton line short"></div>
            <div class="skeleton line medium"></div>
          </div>
        </div>
      </div>

      <p v-else-if="!sessions.length" class="muted">No active sessions recorded.</p>

      <ul v-else class="session-list">
        <li v-for="s in sessions" :key="s.sid" class="session-row">
          <div class="session-meta">
            <div class="session-agent">
              <strong>{{ describeAgent(s.user_agent) }}</strong>
              <span v-if="s.current" class="badge ok">this device</span>
            </div>
            <div class="muted session-where">
              {{ s.ip || 'unknown address' }} · last active {{ formatWhen(s.last_seen_at) }}
            </div>
          </div>
          <button v-if="!s.current" class="ghost sm" :disabled="sessionsBusy"
                  @click="revokeOne(s.sid)">
            Sign out
          </button>
        </li>
      </ul>

      <button v-if="sessions.length > 1" class="danger"
              :disabled="sessionsBusy" style="margin-top: var(--sp-4);"
              @click="revokeOthers">
        Sign out everywhere else
      </button>
    </div>

    <h2 class="section-title">Two-factor authentication</h2>
    <p class="muted">
      Strongly recommended. Without 2FA, anyone who learns your password owns your
      account. With it, they additionally need a code from your phone or a hardware key.
    </p>

    <div class="card stack-card identity-check">
      <h3>Confirm your identity</h3>
      <p class="muted">
        Enter your current password before adding or removing a sign-in factor.
        It is used only for the next action and is never persisted.
      </p>
      <label for="factor-management-pw">Current password for 2FA changes</label>
      <input id="factor-management-pw" v-model="managementPw" type="password"
             autocomplete="current-password" maxlength="256" />
    </div>

    <div v-if="status" class="card stack-card">
      <h3>Status</h3>
      <ul>
        <li>Authenticator app (TOTP): <strong>{{ status.totp_enabled ? 'enabled' : 'disabled' }}</strong></li>
        <li>Passkeys: <strong>{{ status.passkeys_count }}</strong></li>
        <li>Recovery codes remaining: <strong>{{ status.recovery_codes_left }}</strong></li>
      </ul>
    </div>

    <!-- ------- New recovery codes (one-time display) ------- -->
    <div v-if="newCodes.length" class="card attention stack-card">
      <h2 class="card-title">Save your recovery codes</h2>
      <p class="muted">
        Each code works <strong>once</strong> if you lose your authenticator. They will
        not be shown again. Print them, store them in a password manager, or write them on paper.
      </p>
      <!-- Was a <pre> with a hardcoded #f8f8f8 background. The text colour
           came from the theme, so in dark mode these were near-white glyphs
           on a near-white block — the one screen on the site where being
           unreadable means losing the account. -->
      <pre class="code-block">{{ newCodes.join('\n') }}</pre>
      <div class="row tight">
        <button @click="copyCodes">Copy to clipboard</button>
        <label class="check-inline">
          <input type="checkbox" v-model="acceptCodes" /> I have saved them
        </label>
      </div>
      <button :disabled="!acceptCodes" class="ghost" @click="newCodes = []">
        Dismiss
      </button>
    </div>

    <!-- ------- TOTP setup ------- -->
    <div v-if="status && !status.totp_enabled" class="card stack-card">
      <h3>Set up authenticator app</h3>
      <p class="muted">Works with Google Authenticator, 1Password, Authy, Bitwarden, …</p>
      <button v-if="!setupSecret" :disabled="!managementPw" @click="startTotp">
        Begin setup
      </button>
      <div v-if="setupSecret">
        <p>Scan with your authenticator app, or enter the secret manually:</p>
        <img v-if="setupQrSrc" :src="setupQrSrc" alt="QR code for enrolling this account in an authenticator app"
             class="totp-qr" />
        <!-- A 32-character base32 secret has no spaces to break at, so as
             inline <code> it ran past the card on a phone. -->
        <p class="code-block totp-secret">{{ setupSecret }}</p>
        <form @submit.prevent="confirmTotp">
          <label for="totp-confirm">Enter the current 6-digit code to confirm</label>
          <input id="totp-confirm" v-model="setupCode" inputmode="numeric"
                 autocomplete="one-time-code" pattern="[0-9]{6}" maxlength="6" required />
          <button :disabled="loading" style="margin-top: var(--sp-3);">
            {{ loading ? 'Confirming…' : 'Confirm' }}
          </button>
        </form>
      </div>
    </div>

    <!-- ------- Passkey management ------- -->
    <div class="card stack-card">
      <h3>Passkeys</h3>
      <p class="muted">Hardware security keys, Touch ID, Windows Hello, phone passkeys.</p>
      <!-- .inline-form wraps instead of shrinking: side by side the button
           was squeezed to a couple of characters at 320 px. -->
      <form @submit.prevent="addPasskey" class="inline-form">
        <div class="grow">
          <label for="passkey-nick">Nickname (optional)</label>
          <input id="passkey-nick" v-model="passkeyNick" placeholder="MacBook Touch ID" />
        </div>
        <button :disabled="loading || !managementPw">Add passkey</button>
      </form>
      <ul v-if="passkeys.length" class="session-list" style="margin-top: var(--sp-4);">
        <li v-for="p in passkeys" :key="p.id" class="session-row">
          <div class="session-meta">
            <strong>{{ p.nickname || 'Unnamed passkey' }}</strong>
            <div class="muted">added {{ new Date(p.created_at).toLocaleDateString() }}</div>
          </div>
          <button :disabled="!managementPw" @click="removePasskey(p.id)"
                  class="ghost sm danger-text">Remove</button>
        </li>
      </ul>
    </div>

    <!-- ------- Recovery codes regen ------- -->
    <div v-if="status && (status.totp_enabled || status.passkeys_count > 0)" class="card stack-card">
      <h3>Recovery codes</h3>
      <p class="muted">Regenerating invalidates any previous codes.</p>
      <form @submit.prevent="regenerateCodes" class="inline-form">
        <div class="grow">
          <label for="regen-pw">Current password</label>
          <input id="regen-pw" v-model="regenPw" type="password"
                 autocomplete="current-password" required />
        </div>
        <button :disabled="loading">Generate new codes</button>
      </form>
    </div>

    <!-- ------- Disable ------- -->
    <div v-if="status && (status.totp_enabled || status.passkeys_count > 0)"
         class="card destructive stack-card">
      <h3>Disable all 2FA factors</h3>
      <p class="muted">
        Removes TOTP, every passkey, and the recovery codes. Requires your password
        and a current authenticator code so a hijacked session alone cannot do this.
      </p>
      <form @submit.prevent="disableAll">
        <label for="disable-pw">Current password</label>
        <input id="disable-pw" v-model="disablePw" type="password"
               autocomplete="current-password" required />
        <label for="disable-code">Current TOTP code</label>
        <input id="disable-code" v-model="disableCode" inputmode="numeric"
               autocomplete="one-time-code" pattern="[0-9]{6}" maxlength="6" required />
        <button :disabled="loading" class="danger" style="margin-top: var(--sp-4);">
          Disable 2FA
        </button>
      </form>
    </div>

    <p v-if="error" class="error" role="alert" style="margin-top: var(--sp-4);">{{ error }}</p>
  </div>
</template>

<style scoped>
/* Wider than the reading column: this page is a stack of settings panels,
   not prose, and the session rows need room for a device name beside a
   button. */
.security-page { max-width: 45rem; margin-inline: auto; }

.page-title { font-size: var(--step-3); margin: 0 0 var(--sp-5); }
.section-title { font-size: var(--step-2); margin: var(--sp-7) 0 var(--sp-2); }
.card-title { font-size: var(--step-1); margin: 0 0 var(--sp-2); }
.stack-card { margin-top: var(--sp-4); }
.stack-card h3 { font-size: var(--step-1); margin: 0 0 var(--sp-2); }

.session-list { list-style: none; padding: 0; margin: var(--sp-3) 0 0; }
/* Wraps rather than squeezing the device name to nothing when a long user
   agent meets a 320 px screen. */
.session-row {
  display: flex;
  flex-wrap: wrap;
  gap: var(--sp-2) var(--sp-3);
  align-items: center;
  padding: var(--sp-3) 0;
  border-top: 1px solid var(--border);
}
.session-meta { flex: 1 1 12rem; min-width: 0; }
.session-agent { display: flex; flex-wrap: wrap; gap: var(--sp-2); align-items: center; }
.session-where { overflow-wrap: anywhere; }

.check-inline {
  display: flex;
  align-items: center;
  gap: var(--sp-2);
  margin: 0;
  font-size: var(--step--1);
  /* The global `label` rule is block + margin-top, which pushed the
     checkbox onto its own line and off the button's baseline. */
}

.danger-text { color: var(--danger); }
.danger-text:hover { background: var(--danger-soft); color: var(--danger); }

.totp-qr {
  display: block;
  width: 220px;
  max-width: 100%;
  height: auto;
  /* The QR is generated as a data: URI with a white quiet zone. On a dark
     background it needs its own light padding or the outer modules bleed
     into the card and scanners lose the finder patterns. */
  background: #fff;
  padding: var(--sp-2);
  border-radius: var(--radius-sm);
  margin: var(--sp-3) 0;
}
.totp-secret { overflow-wrap: anywhere; }
</style>
