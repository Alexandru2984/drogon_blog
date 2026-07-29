<script setup lang="ts">
import { ref, onMounted } from 'vue'
import { useRoute } from 'vue-router'
import { authApi } from '@/api/auth'

const route = useRoute()
const state = ref<'pending' | 'ok' | 'error'>('pending')
const message = ref('Verifying…')

onMounted(async () => {
  const token = String(route.query.token ?? '')
  if (!token) {
    state.value = 'error'
    message.value = 'Missing token'
    return
  }
  try {
    const res = await authApi.verifyEmail(token)
    state.value = 'ok'
    message.value = res.message ?? 'Email verified'
  } catch (e: any) {
    state.value = 'error'
    message.value = e?.response?.data?.error ?? 'Verification failed'
  }
})
</script>

<template>
  <div class="card auth-card wide" style="text-align: center;">
    <h1 class="auth-title">Email verification</h1>
    <p class="verify-mark" aria-hidden="true">
      {{ state === 'ok' ? '✅' : state === 'error' ? '⚠️' : '⏳' }}
    </p>
    <p :class="state === 'ok' ? 'ok' : state === 'error' ? 'error' : 'muted'"
       :role="state === 'error' ? 'alert' : 'status'">
      {{ message }}
    </p>
    <nav class="auth-links">
      <router-link v-if="state === 'ok'" to="/login" class="btn">Continue to login</router-link>
      <router-link v-else-if="state === 'error'" to="/">Back to the feed</router-link>
    </nav>
  </div>
</template>

<style scoped>
.verify-mark { font-size: var(--step-4); line-height: 1; margin: var(--sp-4) 0 var(--sp-3); }
</style>
