<script setup lang="ts">
import { ref, watch, onMounted, onBeforeUnmount, computed, nextTick } from 'vue'
import { storeToRefs } from 'pinia'
import { useRouter, useRoute, RouterView } from 'vue-router'
import { useAuthStore }     from '@/stores/auth'
import { useToastStore }    from '@/stores/toast'
import { useMessagesStore } from '@/stores/messages'
import ToastList            from '@/components/ToastList.vue'
import LocaleSwitcher       from '@/components/LocaleSwitcher.vue'
import ThemeToggle          from '@/components/ThemeToggle.vue'

const auth     = useAuthStore()
const { user, isAuthed } = storeToRefs(auth)
const toasts   = useToastStore()
const messages = useMessagesStore()
const router   = useRouter()
const route    = useRoute()

const searchInput = ref('')
const drawerOpen  = ref(false)

async function doLogout() {
  drawerOpen.value = false
  messages.disconnectSocket()
  messages.clear()
  await auth.logout()
  toasts.push('Logged out', 'ok')
  router.push({ name: 'home' })
}

function submitSearch() {
  const q = searchInput.value.trim()
  if (!q) return
  drawerOpen.value = false
  router.push({ name: 'search', query: { q } })
}

// The bottom tab bar only exists for signed-in users, and its height has to
// be reserved in the page padding. Driving that from a body class keeps the
// arithmetic in the stylesheet instead of scattering it across components.
watch(isAuthed, (now) => {
  document.body.classList.toggle('has-tabbar', now)
}, { immediate: true })

// A drawer left open across a navigation covers the page the user just
// asked for.
watch(() => route.fullPath, () => { drawerOpen.value = false })

// --- Drawer keyboard handling -------------------------------------------
//
// The drawer is role="dialog" aria-modal="true", which is a promise to a
// keyboard or screen-reader user that focus is inside it and stays there.
// It was not being kept: opening the menu left focus on the toggle, seven
// Tab presses walked out into the page behind the overlay — content the user
// cannot see and did not ask for — and Escape dropped focus wherever it had
// wandered to instead of bringing it back. Each of the three is fixed below.

const drawerEl = ref<HTMLElement | null>(null)
// Where focus was before the drawer opened, so it can be given back.
let lastFocused: HTMLElement | null = null

function focusableIn(root: HTMLElement): HTMLElement[] {
  return Array.from(root.querySelectorAll<HTMLElement>(
    'a[href], button:not([disabled]), input:not([disabled]), select:not([disabled]), ' +
    'textarea:not([disabled]), [tabindex]:not([tabindex="-1"])',
  )).filter(el => el.offsetParent !== null || el === document.activeElement)
}

function onKeydown(e: KeyboardEvent) {
  if (!drawerOpen.value) return

  if (e.key === 'Escape') {
    drawerOpen.value = false
    return
  }

  if (e.key !== 'Tab' || !drawerEl.value) return

  // Cycle within the drawer. Without this the tab order continues into the
  // document behind the overlay.
  const items = focusableIn(drawerEl.value)
  if (!items.length) return
  const first = items[0]
  const last  = items[items.length - 1]
  const active = document.activeElement as HTMLElement | null

  if (e.shiftKey && (active === first || !drawerEl.value.contains(active))) {
    e.preventDefault()
    last.focus()
  } else if (!e.shiftKey && (active === last || !drawerEl.value.contains(active))) {
    e.preventDefault()
    first.focus()
  }
}

// Scroll locking while the drawer is open, so the page behind it does not
// move under the user's finger on a phone; plus focus in on open and back
// out on close.
watch(drawerOpen, async (open) => {
  document.body.style.overflow = open ? 'hidden' : ''

  if (open) {
    lastFocused = document.activeElement as HTMLElement | null
    await nextTick()
    if (drawerEl.value) focusableIn(drawerEl.value)[0]?.focus()
  } else if (lastFocused && document.contains(lastFocused)) {
    // Returning focus to the control that opened the drawer is what keeps a
    // keyboard user's place. If that element is gone (logout removed it),
    // fall back to the document rather than leaving focus on a detached node.
    lastFocused.focus()
    lastFocused = null
  }
})

onMounted(() => {
  window.addEventListener('keydown', onKeydown)
  if (isAuthed.value) {
    messages.connectSocket()
    messages.refreshInbox()
  }
})
onBeforeUnmount(() => {
  window.removeEventListener('keydown', onKeydown)
  document.body.style.overflow = ''
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

const unread = computed(() => messages.totalUnread)
</script>

<template>
  <a class="skip-link" href="#main">Skip to content</a>

  <nav class="navbar" aria-label="Primary">
    <div class="navbar-inner">
      <router-link to="/" class="logo">✦ Micu's Blog</router-link>

      <form class="nav-search" role="search" @submit.prevent="submitSearch">
        <input
          v-model="searchInput"
          type="search"
          :placeholder="$t('nav.search_placeholder')"
          :aria-label="$t('nav.search_aria')"
        />
      </form>

      <div class="nav-links">
        <router-link to="/">{{ $t('nav.feed') }}</router-link>
        <template v-if="isAuthed">
          <router-link to="/posts/new">{{ $t('nav.new_post') }}</router-link>
          <router-link to="/messages">
            {{ $t('nav.messages') }}
            <span v-if="unread" class="nav-badge">{{ unread }}</span>
          </router-link>
          <router-link :to="{ name: 'profile', params: { id: user!.id } }" class="username">
            {{ user!.username }}
          </router-link>
          <router-link to="/account/security">{{ $t('nav.two_fa') }}</router-link>
          <button class="quiet sm" @click="doLogout">{{ $t('nav.logout') }}</button>
        </template>
        <template v-else>
          <router-link to="/login">{{ $t('nav.login') }}</router-link>
          <router-link to="/register">{{ $t('nav.register') }}</router-link>
        </template>
        <ThemeToggle />
        <LocaleSwitcher />
      </div>

      <button
        class="nav-toggle"
        :aria-expanded="drawerOpen"
        aria-controls="mobile-drawer"
        aria-label="Menu"
        @click="drawerOpen = !drawerOpen"
      >
        <span aria-hidden="true">☰</span>
      </button>
    </div>
  </nav>

  <!-- Mobile drawer. Rendered only while open so its links are not in the
       accessibility tree twice alongside the desktop nav. -->
  <template v-if="drawerOpen">
    <div class="drawer-backdrop" @click="drawerOpen = false"></div>
    <div id="mobile-drawer" ref="drawerEl" class="drawer" role="dialog" aria-modal="true" aria-label="Menu">
      <form role="search" @submit.prevent="submitSearch">
        <input
          v-model="searchInput"
          type="search"
          :placeholder="$t('nav.search_placeholder')"
          :aria-label="$t('nav.search_aria')"
        />
      </form>
      <hr />
      <router-link to="/">{{ $t('nav.feed') }}</router-link>
      <template v-if="isAuthed">
        <router-link to="/posts/new">{{ $t('nav.new_post') }}</router-link>
        <router-link to="/messages">
          {{ $t('nav.messages') }}
          <span v-if="unread" class="nav-badge">{{ unread }}</span>
        </router-link>
        <router-link :to="{ name: 'profile', params: { id: user!.id } }">
          {{ user!.username }}
        </router-link>
        <router-link to="/account/security">{{ $t('nav.two_fa') }}</router-link>
        <hr />
        <button @click="doLogout">{{ $t('nav.logout') }}</button>
      </template>
      <template v-else>
        <router-link to="/login">{{ $t('nav.login') }}</router-link>
        <router-link to="/register">{{ $t('nav.register') }}</router-link>
      </template>
      <hr />
      <div class="row">
        <ThemeToggle />
        <LocaleSwitcher />
      </div>
    </div>
  </template>

  <main id="main" class="container">
    <RouterView />
  </main>

  <footer>
    &copy; 2026 Micu's Blog — Built with
    <a href="https://drogon.org" target="_blank" rel="noopener">Drogon</a>
  </footer>

  <!-- Primary destinations within thumb reach on a phone. Signed-in only:
       for a logged-out visitor the feed is the whole app, and a one-item
       bar would just eat screen height. -->
  <nav v-if="isAuthed" class="tabbar" aria-label="Primary (mobile)">
    <router-link to="/">
      <span class="ico" aria-hidden="true">🏠</span>{{ $t('nav.feed') }}
    </router-link>
    <router-link to="/posts/new">
      <span class="ico" aria-hidden="true">✍️</span>{{ $t('nav.new_post') }}
    </router-link>
    <router-link to="/messages">
      <span class="ico" aria-hidden="true">💬</span>{{ $t('nav.messages') }}
      <span v-if="unread" class="nav-badge">{{ unread }}</span>
    </router-link>
    <router-link :to="{ name: 'profile', params: { id: user!.id } }">
      <span class="ico" aria-hidden="true">👤</span>{{ user!.username }}
    </router-link>
  </nav>

  <ToastList :items="toasts.items" />
</template>
