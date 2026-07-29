<script setup lang="ts">
import { ref, computed, onMounted } from 'vue'
import { useRoute, useRouter } from 'vue-router'
import { authApi } from '@/api/auth'
import { useToastStore } from '@/stores/toast'

const route = useRoute()
const router = useRouter()
const toasts = useToastStore()

const token = computed(() => String(route.query.token ?? ''))
const password = ref('')
const confirm = ref('')
const loading = ref(false)
const error = ref('')

onMounted(() => { if (!token.value) error.value = 'Missing reset token' })

async function submit() {
  if (password.value !== confirm.value) { error.value = 'Passwords do not match'; return }
  loading.value = true
  error.value = ''
  try {
    await authApi.resetPassword(token.value, password.value)
    toasts.push('Password updated. Please log in.', 'ok')
    router.push({ name: 'login' })
  } catch (e: any) {
    error.value = e?.response?.data?.error ?? 'Reset failed'
  } finally {
    loading.value = false
  }
}
</script>

<template>
  <div class="card auth-card">
    <h1 class="auth-title">Reset password</h1>
    <form @submit.prevent="submit">
      <label for="reset-pw">New password</label>
      <input id="reset-pw" v-model="password" type="password" required minlength="8"
             autocomplete="new-password" aria-describedby="reset-pw-hint" />
      <p id="reset-pw-hint" class="muted" style="margin-top: var(--sp-2);">
        At least 8 characters, and not one that has appeared in a known breach.
      </p>
      <label for="reset-pw-confirm">Confirm password</label>
      <input id="reset-pw-confirm" v-model="confirm" type="password" required minlength="8"
             autocomplete="new-password" />
      <p v-if="error" class="error" role="alert" style="margin-top: var(--sp-3);">{{ error }}</p>
      <button :disabled="loading || !token" style="margin-top: var(--sp-4); width: 100%;">
        {{ loading ? 'Saving…' : 'Update password' }}
      </button>
    </form>
    <nav class="auth-links">
      <router-link to="/login">Back to login</router-link>
    </nav>
  </div>
</template>
