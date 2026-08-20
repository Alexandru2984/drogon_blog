<script setup lang="ts">
import { computed } from 'vue'
import { useI18n } from 'vue-i18n'
import { useTheme } from '@/composables/useTheme'

const { preference, cycle } = useTheme()
const { t } = useI18n()

const icon = computed(() =>
  preference.value === 'light' ? '☀️'
  : preference.value === 'dark' ? '🌙'
  : '🖥️')

// Announced to screen readers and shown as the tooltip. Naming the *current*
// state rather than the next one, because a control that says "dark" while
// the page is light is ambiguous about which it means.
const label = computed(() =>
  preference.value === 'light' ? t('theme.light')
  : preference.value === 'dark' ? t('theme.dark')
  : t('theme.system'))
</script>

<template>
  <button
    class="quiet sm nav-icon-control"
    :title="label"
    :aria-label="label"
    @click="cycle"
  >
    <span aria-hidden="true">{{ icon }}</span>
  </button>
</template>
