import { defineStore } from 'pinia'
import { ref, computed } from 'vue'
import { authApi, type User } from '@/api/auth'
import { api } from '@/api/client'

interface LoginResponse {
  message?:      string
  user?:         User
  requires_2fa?: boolean
  methods?:      Array<'totp' | 'webauthn' | 'recovery'>
}

export const useAuthStore = defineStore('auth', () => {
  const user = ref<User | null>(null)
  const ready = ref(false)

  // When the backend asks for a second factor, we surface the available
  // methods here so Verify2FAView can render the right tabs.
  const pending2fa = ref<{ methods: Array<'totp' | 'webauthn' | 'recovery'> } | null>(null)

  const isAuthed     = computed(() => user.value !== null)
  const needs2fa     = computed(() => pending2fa.value !== null)

  async function fetchMe() {
    try {
      user.value = await authApi.me()
    } catch {
      user.value = null
    } finally {
      ready.value = true
    }
  }

  // Returns 'authed' when the password was enough, 'pending_2fa' when the
  // backend gated us on a second factor. Caller routes accordingly.
  async function login(username: string, password: string): Promise<'authed' | 'pending_2fa'> {
    const res = (await api.post<LoginResponse>('/auth/login', { username, password })).data
    if (res.requires_2fa) {
      pending2fa.value = { methods: res.methods ?? ['totp', 'recovery'] }
      user.value = null
      return 'pending_2fa'
    }
    pending2fa.value = null
    user.value = res.user ?? null
    return 'authed'
  }

  // Called by Verify2FAView after a successful TOTP / passkey / recovery code.
  function finalizeAfter2fa(u: User) {
    pending2fa.value = null
    user.value = u
  }

  async function register(payload: { username: string; email: string; password: string }) {
    await authApi.register(payload)
  }

  async function logout() {
    try { await authApi.logout() } catch { /* ignore */ }
    pending2fa.value = null
    user.value = null
  }

  function patchUser(p: Partial<User>) {
    if (user.value) user.value = { ...user.value, ...p }
  }

  return { user, ready, isAuthed, pending2fa, needs2fa,
           fetchMe, login, register, logout, patchUser, finalizeAfter2fa }
})
