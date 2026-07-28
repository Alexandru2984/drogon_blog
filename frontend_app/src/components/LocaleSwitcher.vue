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
/* Matches the height and weight of the neighbouring nav controls. A native
   select renders taller than the buttons beside it and with its own font,
   which made it the one element in the header that looked bolted on.
   appearance:none removes the platform chrome so the arrow can be drawn to
   match, and the padding-right leaves room for it. */
.locale-switcher select {
  appearance: none;
  -webkit-appearance: none;
  padding: 0 1.9rem 0 0.65rem;
  height: 2.25rem;
  min-height: 0;
  width: auto;
  border: 1px solid var(--border);
  border-radius: var(--radius);
  background: transparent;
  color: var(--text-dim);
  font: inherit;
  font-size: var(--step--1);
  font-weight: 550;
  cursor: pointer;
  /* Chevron drawn inline so it inherits currentColor and therefore
     follows the theme, unlike a static asset. */
  background-image: url("data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 12 8'%3E%3Cpath d='M1 1l5 5 5-5' fill='none' stroke='%23888' stroke-width='1.6' stroke-linecap='round'/%3E%3C/svg%3E");
  background-repeat: no-repeat;
  background-position: right 0.6rem center;
  background-size: 0.7rem;
  transition: border-color var(--dur, 140ms), color var(--dur, 140ms);
}
.locale-switcher select:hover { color: var(--text); border-color: var(--text-faint); }
.locale-switcher select:focus-visible {
  outline: 2px solid var(--accent);
  outline-offset: 2px;
}
/* The dropdown list itself is drawn by the OS and does not inherit the
   page background, so options need an explicit pair or they render as
   dark-on-dark in dark mode on some platforms. */
.locale-switcher option { background: var(--bg-elev); color: var(--text); }
.visually-hidden {
  position: absolute; width: 1px; height: 1px; padding: 0; margin: -1px;
  overflow: hidden; clip: rect(0, 0, 0, 0); white-space: nowrap; border: 0;
}
</style>
