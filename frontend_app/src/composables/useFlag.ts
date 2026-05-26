import { computed, ref, watch, type ComputedRef } from 'vue'
import { storeToRefs } from 'pinia'
import { flagsApi, type FlagEvalResult } from '@/api/flags'
import { useAuthStore } from '@/stores/auth'

// Lazy-loaded snapshot of every known flag, evaluated server-side
// against the current session. We populate this once per auth change
// — the backend caches inside the process for 30 s already, and
// per-flag re-fetches would burn bandwidth on hot pages.
const flags  = ref<Record<string, boolean>>({})
const ready  = ref(false)
let inflight: Promise<void> | null = null

async function load(): Promise<void> {
  if (inflight) return inflight
  inflight = (async () => {
    try {
      const list = await flagsApi.list()
      const next: Record<string, boolean> = {}
      for (const f of list as FlagEvalResult[]) next[f.key] = f.enabled
      flags.value = next
    } finally {
      ready.value = true
      inflight    = null
    }
  })()
  return inflight
}

// Re-evaluate flags when the user signs in or out — a per-user
// rollout cohort changes with the user id. Watching `isAuthed` here
// is loose: it covers register/login + logout, which are the only
// state transitions that flip the bucket key on this client.
let watcherInstalled = false
function ensureAuthWatcher() {
  if (watcherInstalled) return
  watcherInstalled = true
  const { isAuthed } = storeToRefs(useAuthStore())
  watch(isAuthed, () => {
    ready.value = false
    flags.value = {}
    void load()
  })
}

// Returns a reactive boolean that updates as flags load. Treat
// undefined (pre-load) as off so render paths gating on the flag
// don't flash variant content before the data arrives.
//
// Usage:
//   const isNewFeed = useFlag('new_feed')
//   <feed-redesigned v-if="isNewFeed" />
//   <feed-classic    v-else />
export function useFlag(key: string): ComputedRef<boolean> {
  ensureAuthWatcher()
  if (!ready.value && !inflight) void load()
  return computed(() => Boolean(flags.value[key]))
}

// Force a re-fetch — wire this up to an internal devtools hook if
// you want to flip a flag in PG and see the SPA pick it up without
// a full reload.
export function refreshFlags(): Promise<void> {
  ready.value = false
  inflight    = null
  return load()
}
