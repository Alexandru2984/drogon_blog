<script setup lang="ts">
import { computed } from 'vue'
import { useTheme } from '@/composables/useTheme'

const { preference, cycle } = useTheme()

const icon = computed(() =>
  preference.value === 'light' ? '☀️'
  : preference.value === 'dark' ? '🌙'
  : '🖥️')

// Announced to screen readers and shown as the tooltip. Naming the *current*
// state rather than the next one, because a control that says "dark" while
// the page is light is ambiguous about which it means.
const label = computed(() =>
  preference.value === 'light' ? 'Theme: light'
  : preference.value === 'dark' ? 'Theme: dark'
  : 'Theme: follows your system')
</script>

<template>
  <button
    class="quiet sm"
    :title="label"
    :aria-label="label"
    @click="cycle"
  >
    <span aria-hidden="true">{{ icon }}</span>
  </button>
</template>
