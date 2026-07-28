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
  <!-- aria-live so the announcement reaches a screen reader: a toast is
       often the only feedback that an action worked, and without a live
       region it is invisible to anyone not watching that corner of the
       screen. `polite` rather than `assertive` — these are confirmations,
       not emergencies, and should not interrupt what is being read. -->
  <div class="toast-region" role="status" aria-live="polite" aria-atomic="false">
    <div v-for="t in items" :key="t.id" class="toast" :class="t.kind">{{ t.text }}</div>
  </div>
</template>
