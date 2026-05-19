import { defineStore } from 'pinia'
import { ref, computed } from 'vue'
import { authApi, type User } from '@/api/auth'

export const useAuthStore = defineStore('auth', () => {
  const user = ref<User | null>(null)
  const ready = ref(false)

  const isAuthed = computed(() => user.value !== null)

  async function fetchMe() {
    try {
      user.value = await authApi.me()
    } catch {
      user.value = null
    } finally {
      ready.value = true
    }
  }

  async function login(username: string, password: string) {
    const res = await authApi.login({ username, password })
    user.value = res.user
  }

  async function register(payload: { username: string; email: string; password: string }) {
    await authApi.register(payload)
  }

  async function logout() {
    try { await authApi.logout() } catch { /* ignore */ }
    user.value = null
  }

  function patchUser(p: Partial<User>) {
    if (user.value) user.value = { ...user.value, ...p }
  }

  return { user, ready, isAuthed, fetchMe, login, register, logout, patchUser }
})
