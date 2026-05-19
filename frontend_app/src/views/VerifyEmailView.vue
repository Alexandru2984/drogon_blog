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
  <div class="card" style="max-width: 480px; margin: 2rem auto; text-align: center;">
    <h2>Email verification</h2>
    <p :class="state === 'ok' ? 'ok' : state === 'error' ? 'error' : 'muted'">
      {{ message }}
    </p>
    <p v-if="state === 'ok'"><router-link to="/login">Continue to login →</router-link></p>
  </div>
</template>
