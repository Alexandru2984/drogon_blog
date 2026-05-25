<script setup lang="ts">
// Renders the toast queue. Kept prop-driven (not store-driven) so a
// Storybook isolated render doesn't need a Pinia setup — pass any
// array of items in and the styling matches App.vue. Production
// callers pass `toasts.items` from the Pinia store.

export interface ToastItem {
  id:   number
  text: string
  kind: 'ok' | 'error' | 'info'
}

defineProps<{ items: ToastItem[] }>()
</script>

<template>
  <div v-for="t in items" :key="t.id" class="toast" :class="t.kind">{{ t.text }}</div>
</template>
