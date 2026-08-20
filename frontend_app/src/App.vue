<script setup lang="ts">
import { ref, watch, onMounted, onBeforeUnmount, computed, nextTick } from 'vue'
import { storeToRefs } from 'pinia'
import { useRouter, useRoute, RouterView } from 'vue-router'
import { useI18n } from 'vue-i18n'
import { useAuthStore }     from '@/stores/auth'
import { useToastStore }    from '@/stores/toast'
import { useMessagesStore } from '@/stores/messages'
import { useNotificationsStore } from '@/stores/notifications'
import ToastList            from '@/components/ToastList.vue'
import LocaleSwitcher       from '@/components/LocaleSwitcher.vue'
import ThemeToggle          from '@/components/ThemeToggle.vue'
import AccountMenu          from '@/components/AccountMenu.vue'
import { clearPageMeta, pageMetaOverride } from '@/composables/usePageMeta'

const auth     = useAuthStore()
const { user, isAuthed } = storeToRefs(auth)
const toasts   = useToastStore()
const messages = useMessagesStore()
const notifs   = useNotificationsStore()
const router   = useRouter()
const route    = useRoute()
const { t, locale } = useI18n()

const searchInput = ref('')
const drawerOpen  = ref(false)
const routeAnnouncement = ref('')
const currentYear = new Date().getFullYear()

// Route metadata is translated at runtime, so changing language also updates
// the browser tab. Hash routing keeps API paths and SPA paths from colliding;
// the dedicated /preview/posts/:id endpoint still supplies crawler-facing
// metadata for individual articles.
const genericPageLabel = computed(() => {
  const key = typeof route.meta.titleKey === 'string'
    ? route.meta.titleKey
    : 'pages.default'
  const label = t(key)
  if (route.name === 'tag' && route.params.slug) {
    return `${label}: ${String(route.params.slug)}`
  }
  if (route.name === 'search' && route.query.q) {
    return `${label}: ${String(route.query.q)}`
  }
  return label
})

const pageLabel = computed(() => pageMetaOverride.value?.title || genericPageLabel.value)
const pageDescription = computed(() =>
  pageMetaOverride.value?.description || t('meta.default_description'))

function replaceMetaContent(selector: string, content: string) {
  document.querySelector<HTMLMetaElement>(selector)?.setAttribute('content', content)
}

watch([pageLabel, pageDescription, locale], ([label, description]) => {
  document.title = `${label} · Micu's Blog`
  replaceMetaContent('meta[name="description"]', description)
  replaceMetaContent('meta[property="og:title"]', label)
  replaceMetaContent('meta[property="og:description"]', description)
  replaceMetaContent('meta[name="twitter:title"]', label)
  replaceMetaContent('meta[name="twitter:description"]', description)
}, { immediate: true })

// A visual route transition is otherwise silent in an SPA. Clear first so
// navigating between two parameterized routes with the same generic title is
// announced as well.
watch(() => route.fullPath, async () => {
  clearPageMeta()
  routeAnnouncement.value = ''
  await nextTick()
  routeAnnouncement.value = t('a11y.page_loaded', { page: pageLabel.value })
}, { immediate: true })

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
  // The socket delivers a nudge; the store does the refetch. Registered once
  // here rather than in every view that cares about the badge.
  messages.setNotificationHandler(() => notifs.refreshCount())
  if (isAuthed.value) {
    messages.connectSocket()
    messages.refreshInbox()
    notifs.refreshCount()
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
    notifs.refreshCount()
  } else if (!now && prev) {
    messages.disconnectSocket()
    messages.clear()
    notifs.clear()
  }
})

const unread = computed(() => messages.totalUnread)
const unreadNotifs = computed(() => notifs.unread)
</script>

<template>
  <a class="skip-link" href="#main">{{ $t('a11y.skip_to_content') }}</a>

  <p class="visually-hidden" role="status" aria-live="polite" aria-atomic="true">
    {{ routeAnnouncement }}
  </p>

  <nav class="navbar" :aria-label="$t('a11y.primary_navigation')">
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
        <router-link :to="{ name: 'tags' }">{{ $t('nav.tags') }}</router-link>
        <template v-if="isAuthed">
          <!-- Only the destinations a reader moves between often stay on the
               bar. Everything account-scoped lives under the name it belongs
               to — twelve top-level items did not fit at 1440 px. -->
          <router-link to="/posts/new">{{ $t('nav.new_post') }}</router-link>
          <router-link :to="{ name: 'notifications' }">
            {{ $t('nav.notifications') }}
            <span v-if="unreadNotifs" class="nav-badge">{{ unreadNotifs }}</span>
          </router-link>
          <router-link to="/messages">
            {{ $t('nav.messages') }}
            <span v-if="unread" class="nav-badge">{{ unread }}</span>
          </router-link>
          <AccountMenu :username="user!.username" @logout="doLogout">
            <router-link :to="{ name: 'profile', params: { id: user!.id } }" role="menuitem">
              {{ $t('nav.profile') }}
            </router-link>
            <router-link :to="{ name: 'drafts' }" role="menuitem">{{ $t('nav.drafts') }}</router-link>
            <router-link :to="{ name: 'bookmarks' }" role="menuitem">{{ $t('nav.saved') }}</router-link>
            <router-link to="/account/security" role="menuitem">{{ $t('nav.two_fa') }}</router-link>
            <router-link :to="{ name: 'account-data' }" role="menuitem">
              {{ $t('nav.your_data') }}
            </router-link>
            <template #logout-label>{{ $t('nav.logout') }}</template>
          </AccountMenu>
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
        :aria-label="$t('a11y.open_menu')"
        @click="drawerOpen = !drawerOpen"
      >
        <span aria-hidden="true">☰</span>
      </button>
    </div>
  </nav>

  <!-- Mobile drawer. Rendered only while open so its links are not in the
       accessibility tree twice alongside the desktop nav. -->
  <template v-if="drawerOpen">
    <div class="drawer-backdrop" aria-hidden="true" @click="drawerOpen = false"></div>
    <div id="mobile-drawer" ref="drawerEl" class="drawer" role="dialog" aria-modal="true"
         aria-labelledby="mobile-drawer-title">
      <header class="drawer-head">
        <h2 id="mobile-drawer-title">{{ $t('a11y.menu') }}</h2>
        <button class="quiet drawer-close" :aria-label="$t('a11y.close_menu')"
                @click="drawerOpen = false">
          <span aria-hidden="true">×</span>
        </button>
      </header>
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
      <router-link :to="{ name: 'tags' }">{{ $t('nav.tags') }}</router-link>
      <template v-if="isAuthed">
        <router-link to="/posts/new">{{ $t('nav.new_post') }}</router-link>
        <router-link :to="{ name: 'drafts' }">{{ $t('nav.drafts') }}</router-link>
        <router-link :to="{ name: 'bookmarks' }">{{ $t('nav.saved') }}</router-link>
        <router-link :to="{ name: 'notifications' }">
          {{ $t('nav.notifications') }}
          <span v-if="unreadNotifs" class="nav-badge">{{ unreadNotifs }}</span>
        </router-link>
        <router-link to="/messages">
          {{ $t('nav.messages') }}
          <span v-if="unread" class="nav-badge">{{ unread }}</span>
        </router-link>
        <router-link :to="{ name: 'profile', params: { id: user!.id } }">
          {{ user!.username }}
        </router-link>
        <router-link to="/account/security">{{ $t('nav.two_fa') }}</router-link>
        <router-link :to="{ name: 'account-data' }">{{ $t('nav.your_data') }}</router-link>
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

  <main id="main" class="container" :class="{ wide: route.meta?.wide }" tabindex="-1">
    <RouterView />
  </main>

  <footer>
    &copy; {{ currentYear }} Micu's Blog — {{ $t('footer.built_with') }}
    <a href="https://drogon.org" target="_blank" rel="noopener">Drogon</a>
  </footer>

  <!-- Primary destinations within thumb reach on a phone. Signed-in only:
       for a logged-out visitor the feed is the whole app, and a one-item
       bar would just eat screen height. -->
  <nav v-if="isAuthed" class="tabbar" :aria-label="$t('a11y.mobile_navigation')">
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
