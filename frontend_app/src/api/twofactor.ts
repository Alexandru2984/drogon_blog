import { api } from './client'

// 2FA management + two-step login completion. The browser-side WebAuthn
// ceremonies (navigator.credentials.create / get) are not in this file —
// see `webauthnRegister()` / `webauthnAuthenticate()` in
// `frontend_app/src/views/Security2FAView.vue` and `Verify2FAView.vue`.

export interface Status2fa {
  totp_enabled:        boolean
  passkeys_count:      number
  recovery_codes_left: number
}

export interface SetupTotpResponse {
  secret:      string
  otpauth_url: string
}

export interface ConfirmTotpResponse {
  enabled:        boolean
  recovery_codes: string[]
}

export interface Passkey {
  id:           number
  nickname:     string
  created_at:   string
  last_used_at: string | null
}

export const twoFactorApi = {
  status() { return api.get<Status2fa>('/auth/2fa/status').then(r => r.data) },

  totpSetup(password: string) {
    return api.post<SetupTotpResponse>(
      '/auth/2fa/totp/setup', { password }).then(r => r.data)
  },
  totpConfirm(code: string) {
    return api.post<ConfirmTotpResponse>('/auth/2fa/totp/confirm', { code }).then(r => r.data)
  },
  disable(payload: { password: string; totp_code: string }) {
    return api.post<{ enabled: false }>('/auth/2fa/disable', payload).then(r => r.data)
  },
  regenerateRecoveryCodes(password: string) {
    return api.post<{ recovery_codes: string[] }>(
      '/auth/2fa/recovery-codes/regenerate', { password }).then(r => r.data)
  },

  webauthnRegisterBegin(password: string) {
    return api.post(
      '/auth/2fa/webauthn/register/begin', { password }).then(r => r.data)
  },
  webauthnRegisterFinish(payload: {
    clientDataJSON:    string
    attestationObject: string
    nickname?:         string
  }) {
    return api.post('/auth/2fa/webauthn/register/finish', payload).then(r => r.data)
  },
  webauthnList() {
    return api.get<{ credentials: Passkey[] }>('/auth/2fa/webauthn/list').then(r => r.data)
  },
  webauthnRemove(id: number, password: string) {
    return api.post(`/auth/2fa/webauthn/remove/${id}`, { password }).then(r => r.data)
  },

  // ---- Two-step login completion ----
  verifyTotp(code: string) {
    return api.post('/auth/login/verify-totp', { code }).then(r => r.data)
  },
  verifyRecovery(code: string) {
    return api.post('/auth/login/verify-recovery', { code }).then(r => r.data)
  },
  webauthnLoginBegin() {
    return api.post('/auth/login/verify-webauthn/begin').then(r => r.data)
  },
  webauthnLoginFinish(payload: {
    credentialId:        string
    clientDataJSON:      string
    authenticatorData:   string
    signature:           string
  }) {
    return api.post('/auth/login/verify-webauthn/finish', payload).then(r => r.data)
  },
}

// ===========================================================================
// Browser WebAuthn helpers. Convert between the spec's ArrayBuffer fields and
// the base64url-encoded JSON the backend exchanges.
// ===========================================================================

const b64u = {
  encode(buf: ArrayBuffer): string {
    const bytes = new Uint8Array(buf)
    let bin = ''
    for (const b of bytes) bin += String.fromCharCode(b)
    return btoa(bin).replace(/\+/g, '-').replace(/\//g, '_').replace(/=+$/, '')
  },
  // Returns ArrayBuffer (the spec demands BufferSource — passing a
  // Uint8Array views into a SharedArrayBuffer-typed buffer would not
  // satisfy `PublicKeyCredentialDescriptor.id`'s narrowed type).
  decode(s: string): ArrayBuffer {
    s = s.replace(/-/g, '+').replace(/_/g, '/')
    while (s.length % 4) s += '='
    const bin = atob(s)
    const out = new ArrayBuffer(bin.length)
    const view = new Uint8Array(out)
    for (let i = 0; i < bin.length; ++i) view[i] = bin.charCodeAt(i)
    return out
  },
}

interface ServerCredDescriptor { id: string; type: 'public-key' }
interface ServerRegisterOptions {
  challenge:          string
  rp:                 { id: string; name: string }
  user:               { id: string; name: string; displayName: string }
  pubKeyCredParams:   Array<{ type: 'public-key'; alg: number }>
  attestation:        AttestationConveyancePreference
  excludeCredentials: ServerCredDescriptor[]
}

interface ServerAssertOptions {
  challenge:        string
  rp_id:            string
  allowCredentials: ServerCredDescriptor[]
}

export async function webauthnRegister(nickname: string, password: string) {
  const opts = (
    await twoFactorApi.webauthnRegisterBegin(password)
  ) as ServerRegisterOptions

  const publicKey: PublicKeyCredentialCreationOptions = {
    challenge:        b64u.decode(opts.challenge),
    rp:               opts.rp,
    user: {
      id:          b64u.decode(opts.user.id),
      name:        opts.user.name,
      displayName: opts.user.displayName,
    },
    pubKeyCredParams: opts.pubKeyCredParams,
    attestation:      opts.attestation,
    excludeCredentials: opts.excludeCredentials.map(c => ({
      type: c.type, id: b64u.decode(c.id),
    })),
    authenticatorSelection: { userVerification: 'preferred' },
    timeout: 60_000,
  }

  const cred = await navigator.credentials.create({ publicKey }) as PublicKeyCredential | null
  if (!cred) throw new Error('WebAuthn registration was cancelled')
  const att = cred.response as AuthenticatorAttestationResponse

  return await twoFactorApi.webauthnRegisterFinish({
    clientDataJSON:    b64u.encode(att.clientDataJSON),
    attestationObject: b64u.encode(att.attestationObject),
    nickname,
  })
}

export async function webauthnAuthenticate() {
  const opts = (await twoFactorApi.webauthnLoginBegin()) as ServerAssertOptions

  const publicKey: PublicKeyCredentialRequestOptions = {
    challenge:        b64u.decode(opts.challenge),
    rpId:             opts.rp_id,
    allowCredentials: opts.allowCredentials.map(c => ({
      type: c.type, id: b64u.decode(c.id),
    })),
    userVerification: 'preferred',
    timeout: 60_000,
  }

  const cred = await navigator.credentials.get({ publicKey }) as PublicKeyCredential | null
  if (!cred) throw new Error('WebAuthn authentication was cancelled')
  const ass = cred.response as AuthenticatorAssertionResponse

  return await twoFactorApi.webauthnLoginFinish({
    credentialId:      cred.id,                                  // already base64url
    clientDataJSON:    b64u.encode(ass.clientDataJSON),
    authenticatorData: b64u.encode(ass.authenticatorData),
    signature:         b64u.encode(ass.signature),
  })
}
