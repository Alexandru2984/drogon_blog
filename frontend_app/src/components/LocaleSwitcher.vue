<script setup lang="ts">
import { useI18n } from 'vue-i18n'
import { setLocale, SUPPORTED_LOCALES, type Locale } from '@/i18n'

const { locale, t } = useI18n()

function onChange(e: Event) {
  const next = (e.target as HTMLSelectElement).value as Locale
  setLocale(next)
}
</script>

<template>
  <label class="locale-switcher">
    <span class="visually-hidden">{{ t('locale.label') }}</span>
    <select :value="locale" @change="onChange" :aria-label="t('locale.label')">
      <option v-for="l in SUPPORTED_LOCALES" :key="l" :value="l">
        {{ t('locale.' + l) }}
      </option>
    </select>
  </label>
</template>

<style scoped>
.locale-switcher select {
  padding: 0.25rem 0.5rem;
  border:  1px solid var(--border, #ddd);
  border-radius: 6px;
  background: transparent;
  font-size: 0.85em;
  cursor: pointer;
}
.visually-hidden {
  position: absolute; width: 1px; height: 1px; padding: 0; margin: -1px;
  overflow: hidden; clip: rect(0, 0, 0, 0); white-space: nowrap; border: 0;
}
</style>
