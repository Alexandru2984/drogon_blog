import { defineStore } from 'pinia'
import { ref } from 'vue'
import { socialApi, type AppNotification } from '@/api/social'
import { useAuthStore } from './auth'

// Notification state lives in a store rather than in the bell component
// because two places need it: the navigation badge, which is mounted for the
// whole session, and the notification list, which is not.
export const useNotificationsStore = defineStore('notifications', () => {
  const items       = ref<AppNotification[]>([])
  const unread      = ref(0)
  const loading     = ref(false)
  const loadingMore = ref(false)
  const nextCursor  = ref<number | null>(null)

  const auth = useAuthStore()

  async function refreshCount() {
    if (!auth.isAuthed) { unread.value = 0; return }
    try {
      unread.value = await socialApi.unreadCount()
    } catch {
      // A wrong badge is better than a page that will not render.
    }
  }

  async function load() {
    if (!auth.isAuthed) return
    loading.value = true
    try {
      const res = await socialApi.notifications(undefined, 30)
      items.value      = res.notifications
      unread.value     = res.unread
      nextCursor.value = res.next_cursor ?? null
    } finally {
      loading.value = false
    }
  }

  async function loadMore() {
    if (!auth.isAuthed || loadingMore.value || !nextCursor.value) return
    loadingMore.value = true
    try {
      const res = await socialApi.notifications(nextCursor.value, 30)
      const seen = new Set(items.value.map(n => n.id))
      for (const n of res.notifications) {
        if (!seen.has(n.id)) {
          items.value.push(n)
          seen.add(n.id)
        }
      }
      unread.value     = res.unread
      nextCursor.value = res.next_cursor ?? null
    } finally {
      loadingMore.value = false
    }
  }

  async function markRead(id: number) {
    const n = items.value.find(x => x.id === id)
    if (!n || n.read) return
    n.read = true
    unread.value = Math.max(0, unread.value - 1)
    try {
      unread.value = await socialApi.markRead(id)
    } catch {
      n.read = false
      await refreshCount()
    }
  }

  async function markAllRead() {
    const previously = items.value.filter(n => !n.read)
    for (const n of previously) n.read = true
    unread.value = 0
    try {
      await socialApi.markAllRead()
    } catch {
      for (const n of previously) n.read = false
      await refreshCount()
    }
  }

  function clear() {
    items.value = []
    unread.value = 0
    nextCursor.value = null
  }

  return {
    items, unread, loading, loadingMore, nextCursor,
    refreshCount, load, loadMore, markRead, markAllRead, clear,
  }
})
