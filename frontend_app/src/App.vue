<script setup lang="ts">
import { ref, watch, onMounted } from 'vue'
import { storeToRefs } from 'pinia'
import { useRouter, RouterView } from 'vue-router'
import { useAuthStore }     from '@/stores/auth'
import { useToastStore }    from '@/stores/toast'
import { useMessagesStore } from '@/stores/messages'
import ToastList            from '@/components/ToastList.vue'

const auth     = useAuthStore()
const { user, isAuthed } = storeToRefs(auth)
const toasts   = useToastStore()
const messages = useMessagesStore()
const router   = useRouter()

const searchInput = ref('')

async function doLogout() {
  messages.disconnectSocket()
  messages.clear()
  await auth.logout()
  toasts.push('Logged out', 'ok')
  router.push({ name: 'home' })
}

function submitSearch() {
  const q = searchInput.value.trim()
  if (!q) return
  router.push({ name: 'search', query: { q } })
}

// Open the WebSocket as soon as we know the user is authenticated (whether
// from /auth/me at boot or after a fresh login) and tear it down on logout.
onMounted(() => {
  if (isAuthed.value) {
    messages.connectSocket()
    messages.refreshInbox()
  }
})
watch(isAuthed, (now, prev) => {
  if (now && !prev) {
    messages.connectSocket()
    messages.refreshInbox()
  } else if (!now && prev) {
    messages.disconnectSocket()
    messages.clear()
  }
})
</script>

<template>
  <nav class="navbar">
    <div class="navbar-inner">
      <router-link to="/" class="logo">✦ Micu's Blog</router-link>
      <form class="nav-search" @submit.prevent="submitSearch">
        <input
          v-model="searchInput"
          type="search"
          placeholder="Search posts…"
          aria-label="Search posts"
        />
      </form>
      <div class="nav-links">
        <router-link to="/">Feed</router-link>
        <template v-if="isAuthed">
          <router-link to="/posts/new">New post</router-link>
          <router-link to="/messages" class="nav-messages">
            Messages
            <span v-if="messages.totalUnread" class="nav-badge">{{ messages.totalUnread }}</span>
          </router-link>
          <router-link :to="{ name: 'profile', params: { id: user!.id } }" class="username">
            {{ user!.username }}
          </router-link>
          <router-link to="/account/security" class="ghost-link" title="Two-factor authentication">2FA</router-link>
          <button class="ghost" @click="doLogout">Logout</button>
        </template>
        <template v-else>
          <router-link to="/login">Login</router-link>
          <router-link to="/register">Register</router-link>
        </template>
      </div>
    </div>
  </nav>

  <main class="container">
    <RouterView />
  </main>

  <footer>
    &copy; 2026 Micu's Blog — Built with
    <a href="https://drogon.org" target="_blank" rel="noopener">Drogon</a>
  </footer>

  <ToastList :items="toasts.items" />
</template>
