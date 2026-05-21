<script setup lang="ts">
import { ref } from 'vue'
import { useRouter, useRoute } from 'vue-router'
import { useAuthStore } from '@/stores/auth'
import { useToastStore } from '@/stores/toast'

const username = ref('')
const password = ref('')
const loading = ref(false)
const error = ref('')

const auth = useAuthStore()
const router = useRouter()
const route = useRoute()
const toasts = useToastStore()

// Reject anything that could navigate off the current origin. Protocol-relative
// URLs (`//evil.example`) and absolute URLs both fall through to '/'.
function safeNext(raw: unknown): string {
  if (typeof raw !== 'string' || raw.length === 0) return '/'
  if (raw[0] !== '/' || raw.startsWith('//') || raw.startsWith('/\\')) return '/'
  return raw
}

async function submit() {
  error.value = ''
  loading.value = true
  try {
    const outcome = await auth.login(username.value, password.value)
    if (outcome === 'pending_2fa') {
      // The backend gated us on a second factor — hop to the verification
      // view, preserving the original `next` redirect.
      const next = safeNext(route.query.next)
      router.push({ path: '/login/2fa', query: { next } })
      return
    }
    toasts.push(`Welcome back, ${auth.user!.username}`, 'ok')
    router.push(safeNext(route.query.next))
  } catch (e: any) {
    error.value = e?.response?.data?.error ?? 'Login failed'
  } finally {
    loading.value = false
  }
}
</script>

<template>
  <div class="card" style="max-width: 420px; margin: 2rem auto;">
    <h2>Login</h2>
    <form @submit.prevent="submit">
      <label for="login-username">Username</label>
      <input id="login-username" v-model="username" autofocus autocomplete="username" required />
      <label for="login-password">Password</label>
      <input id="login-password" v-model="password" type="password" autocomplete="current-password" required />
      <p v-if="error" class="error" style="margin-top: 0.75rem;">{{ error }}</p>
      <button :disabled="loading" style="margin-top: 1rem; width: 100%;">
        {{ loading ? 'Signing in…' : 'Sign in' }}
      </button>
    </form>
    <p class="muted" style="margin-top: 1rem; text-align: center;">
      <router-link to="/forgot-password">Forgot password?</router-link>
      &nbsp;·&nbsp;
      <router-link to="/register">Create account</router-link>
    </p>
  </div>
</template>
