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
  <div class="card" style="max-width: 420px; margin: 2rem auto;">
    <h2>Reset password</h2>
    <form @submit.prevent="submit">
      <label>New password</label>
      <input v-model="password" type="password" required minlength="8" autocomplete="new-password" />
      <label>Confirm password</label>
      <input v-model="confirm" type="password" required minlength="8" />
      <p v-if="error" class="error" style="margin-top: 0.75rem;">{{ error }}</p>
      <button :disabled="loading || !token" style="margin-top: 1rem; width: 100%;">
        {{ loading ? 'Saving…' : 'Update password' }}
      </button>
    </form>
  </div>
</template>
