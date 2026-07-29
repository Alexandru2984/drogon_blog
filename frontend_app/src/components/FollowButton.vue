<script setup lang="ts">
import { ref, onMounted, watch } from 'vue'
import { socialApi } from '@/api/social'
import { useAuthStore } from '@/stores/auth'
import { useToastStore } from '@/stores/toast'

const props = defineProps<{ userId: number }>()

const auth   = useAuthStore()
const toasts = useToastStore()

const stats     = ref<{ followers: number; following: number; is_following: boolean } | null>(null)
const busy      = ref(false)
const isMe      = ref(false)

async function load() {
  isMe.value = auth.isAuthed && auth.user!.id === props.userId
  try {
    stats.value = await socialApi.followStats(props.userId)
  } catch {
    // The counts are supporting information; a profile that renders without
    // them is better than a profile that fails to render.
    stats.value = null
  }
}

onMounted(load)
watch(() => props.userId, load)

async function toggle() {
  if (!auth.isAuthed || busy.value || !stats.value) return
  const was = stats.value.is_following
  busy.value = true
  // Optimistic, including the count, so the number does not sit still for a
  // round trip after the button has visibly changed.
  stats.value.is_following = !was
  stats.value.followers += was ? -1 : 1
  try {
    if (was) await socialApi.unfollow(props.userId)
    else     await socialApi.follow(props.userId)
  } catch (e: any) {
    stats.value.is_following = was
    stats.value.followers += was ? 1 : -1
    toasts.push(e?.response?.data?.error ?? 'Could not update', 'error')
  } finally {
    busy.value = false
  }
}
</script>

<template>
  <div v-if="stats" class="follow">
    <button
      v-if="auth.isAuthed && !isMe"
      :class="stats.is_following ? 'ghost' : ''"
      :disabled="busy"
      :aria-pressed="stats.is_following"
      @click="toggle"
    >
      {{ stats.is_following ? 'Following' : 'Follow' }}
    </button>

    <p class="muted follow-counts">
      <!-- Counts are public and worth showing to a signed-out reader too;
           only the button needs an account. -->
      <strong>{{ stats.followers }}</strong>
      follower{{ stats.followers === 1 ? '' : 's' }}
      <span aria-hidden="true"> · </span>
      <strong>{{ stats.following }}</strong> following
    </p>
  </div>
</template>

<style scoped>
.follow {
  display: flex;
  flex-wrap: wrap;
  align-items: center;
  gap: var(--sp-2) var(--sp-3);
}
.follow-counts { margin: 0; }
.follow-counts strong { color: var(--text); font-variant-numeric: tabular-nums; }
</style>
