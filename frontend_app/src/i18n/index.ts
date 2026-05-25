import { createI18n } from 'vue-i18n'
import en from '../locales/en.json'
import ro from '../locales/ro.json'

export type Locale = 'en' | 'ro'
export const SUPPORTED_LOCALES: Locale[] = ['en', 'ro']
const STORAGE_KEY = 'locale'

// Pick an initial locale: explicit user choice from localStorage wins;
// then the browser's first-language match; then fall back to English.
// Kept side-effect-free so importing this module from non-browser
// contexts (Storybook stub, SSR if added later) doesn't crash.
function resolveInitial(): Locale {
  try {
    const stored = typeof localStorage !== 'undefined'
      ? localStorage.getItem(STORAGE_KEY)
      : null
    if (stored && SUPPORTED_LOCALES.includes(stored as Locale)) {
      return stored as Locale
    }
    if (typeof navigator !== 'undefined' && navigator.language) {
      const head = navigator.language.slice(0, 2).toLowerCase()
      if (SUPPORTED_LOCALES.includes(head as Locale)) return head as Locale
    }
  } catch { /* no-op — quotes-exceeded / disabled storage */ }
  return 'en'
}

export const i18n = createI18n({
  // legacy:false unlocks the Composition API (`useI18n()`). The
  // Options API ($t) keeps working in templates either way.
  legacy:         false,
  // Throwing-on-missing would be too noisy during incremental
  // translation; for now we silently fall back to the key string,
  // which makes untranslated entries visible without breaking the UI.
  missingWarn:    false,
  fallbackWarn:   false,
  globalInjection: true,
  locale:         resolveInitial(),
  fallbackLocale: 'en',
  messages:       { en, ro },
})

// Public switcher used by LocaleSwitcher.vue. Centralised so localStorage
// + i18n state stay in lockstep (forgetting one led to UI/storage
// divergence in past projects).
export function setLocale(next: Locale): void {
  i18n.global.locale.value = next
  try { localStorage.setItem(STORAGE_KEY, next) } catch { /* ignore */ }
  // Reflect on <html lang> so screen readers + browser spellcheck
  // pick the right language pack.
  if (typeof document !== 'undefined') {
    document.documentElement.lang = next
  }
}

// Initial <html lang> sync on module load, since setLocale isn't
// called for the first paint.
if (typeof document !== 'undefined') {
  document.documentElement.lang = i18n.global.locale.value as string
}
