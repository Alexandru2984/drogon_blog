<script setup lang="ts">
import { ref } from 'vue'
import { authApi } from '@/api/auth'

const email = ref('')
const loading = ref(false)
const message = ref('')
const isError = ref(false)

async function submit() {
  loading.value = true
  message.value = ''
  isError.value = false
  try {
    const res = await authApi.requestReset(email.value)
    message.value = res.message ?? 'If an account exists, a reset link has been sent.'
  } catch (e: any) {
    isError.value = true
    message.value = e?.response?.data?.error ?? 'Request failed'
  } finally {
    loading.value = false
  }
}
</script>

<template>
  <div class="card auth-card">
    <h1 class="auth-title">{{ $t('auth.forgot_heading') }}</h1>
    <form @submit.prevent="submit">
      <label for="forgot-email">{{ $t('auth.email') }}</label>
      <input id="forgot-email" v-model="email" type="email" autocomplete="email" required autofocus />
      <button :disabled="loading" style="margin-top: var(--sp-4); width: 100%;">
        {{ loading ? $t('common.loading') : $t('auth.send_reset') }}
      </button>
    </form>
    <p v-if="message" :class="isError ? 'error' : 'ok'" :role="isError ? 'alert' : 'status'"
       style="margin-top: var(--sp-4);">{{ message }}</p>
    <nav class="auth-links">
      <router-link to="/login">{{ $t('auth.log_in_link') }}</router-link>
    </nav>
  </div>
</template>
