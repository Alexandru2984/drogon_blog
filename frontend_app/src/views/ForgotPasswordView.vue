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
  <div class="card" style="max-width: 420px; margin: 2rem auto;">
    <h2>{{ $t('auth.forgot_heading') }}</h2>
    <form @submit.prevent="submit">
      <label>{{ $t('auth.email') }}</label>
      <input v-model="email" type="email" required autofocus />
      <button :disabled="loading" style="margin-top: 1rem; width: 100%;">
        {{ loading ? $t('common.loading') : $t('auth.send_reset') }}
      </button>
    </form>
    <p v-if="message" :class="isError ? 'error' : 'ok'" style="margin-top: 1rem;">{{ message }}</p>
  </div>
</template>
