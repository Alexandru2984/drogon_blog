<script setup lang="ts">
import { ref } from 'vue'
import { useRouter } from 'vue-router'
import { useAuthStore } from '@/stores/auth'
import { useToastStore } from '@/stores/toast'

const username = ref('')
const email = ref('')
const password = ref('')
const loading = ref(false)
const error = ref('')
const done = ref(false)

const auth = useAuthStore()
const router = useRouter()
const toasts = useToastStore()

async function submit() {
  error.value = ''
  loading.value = true
  try {
    await auth.register({ username: username.value, email: email.value, password: password.value })
    done.value = true
    toasts.push('Check your email to verify your account', 'ok')
    setTimeout(() => router.push({ name: 'login' }), 1500)
  } catch (e: any) {
    error.value = e?.response?.data?.error ?? 'Registration failed'
  } finally {
    loading.value = false
  }
}
</script>

<template>
  <div class="card" style="max-width: 420px; margin: 2rem auto;">
    <h2>Create account</h2>
    <form v-if="!done" @submit.prevent="submit">
      <label for="reg-username">Username</label>
      <input id="reg-username" v-model="username" autofocus required minlength="3" maxlength="64" />
      <label for="reg-email">Email</label>
      <input id="reg-email" v-model="email" type="email" required />
      <label for="reg-password">Password</label>
      <input id="reg-password" v-model="password" type="password" autocomplete="new-password" required minlength="8" />
      <p v-if="error" class="error" style="margin-top: 0.75rem;">{{ error }}</p>
      <button :disabled="loading" style="margin-top: 1rem; width: 100%;">
        {{ loading ? 'Creating…' : 'Sign up' }}
      </button>
    </form>
    <p v-else class="ok">
      Account created. Please check your email to verify your address.
    </p>
    <p class="muted" style="margin-top: 1rem; text-align: center;">
      Already have one? <router-link to="/login">Log in</router-link>
    </p>
  </div>
</template>
