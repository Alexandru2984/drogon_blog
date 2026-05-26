<script setup lang="ts">
import { ref, onBeforeUnmount } from 'vue'
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

// Held so the redirect-to-login setTimeout can be cancelled when the
// component unmounts (the user navigated away on their own, or in
// tests when registerAndLogin synthetically drives login immediately
// after registration). Without this, the timer kept firing 1.5 s
// later and kicked the user off whatever route they had reached.
let redirectTimer: ReturnType<typeof setTimeout> | null = null

onBeforeUnmount(() => {
  if (redirectTimer) { clearTimeout(redirectTimer); redirectTimer = null }
})

async function submit() {
  error.value = ''
  loading.value = true
  try {
    await auth.register({ username: username.value, email: email.value, password: password.value })
    done.value = true
    toasts.push('Check your email to verify your account', 'ok')
    redirectTimer = setTimeout(() => router.push({ name: 'login' }), 1500)
  } catch (e: any) {
    error.value = e?.response?.data?.error ?? 'Registration failed'
  } finally {
    loading.value = false
  }
}
</script>

<template>
  <div class="card" style="max-width: 420px; margin: 2rem auto;">
    <h2>{{ $t('auth.register_heading') }}</h2>
    <form v-if="!done" @submit.prevent="submit">
      <label for="reg-username">{{ $t('auth.username') }}</label>
      <input id="reg-username" v-model="username" autofocus required minlength="3" maxlength="64" />
      <label for="reg-email">{{ $t('auth.email') }}</label>
      <input id="reg-email" v-model="email" type="email" required />
      <label for="reg-password">{{ $t('auth.password') }}</label>
      <input id="reg-password" v-model="password" type="password" autocomplete="new-password" required minlength="8" />
      <p v-if="error" class="error" style="margin-top: 0.75rem;">{{ error }}</p>
      <button :disabled="loading" style="margin-top: 1rem; width: 100%;">
        {{ loading ? $t('common.loading') : $t('auth.sign_up') }}
      </button>
    </form>
    <p v-else class="ok">
      {{ $t('auth.verify_email_hint') }}
    </p>
    <p class="muted" style="margin-top: 1rem; text-align: center;">
      <router-link to="/login">{{ $t('auth.log_in_link') }}</router-link>
    </p>
  </div>
</template>
