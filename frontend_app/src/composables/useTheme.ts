import { ref, watchEffect } from 'vue'

export type ThemePreference = 'system' | 'light' | 'dark'

const STORAGE_KEY = 'blog:theme'

function readStored(): ThemePreference {
  try {
    const v = localStorage.getItem(STORAGE_KEY)
    if (v === 'light' || v === 'dark' || v === 'system') return v
  } catch {
    // Private browsing and some embedded webviews throw on localStorage
    // access rather than returning null. Falling back to 'system' keeps
    // the site usable instead of breaking at boot.
  }
  return 'system'
}

const preference = ref<ThemePreference>(readStored())

/**
 * Theme preference, persisted.
 *
 * 'system' deliberately writes *no* attribute, leaving the stylesheet's
 * `prefers-color-scheme` block in charge. Stamping an explicit value would
 * freeze the theme at whatever the OS happened to be at page load, so a
 * user who switches their OS to dark in the evening would stay on light
 * until they reloaded.
 */
export function useTheme() {
  watchEffect(() => {
    const root = document.documentElement
    if (preference.value === 'system') root.removeAttribute('data-theme')
    else root.setAttribute('data-theme', preference.value)

    try {
      localStorage.setItem(STORAGE_KEY, preference.value)
    } catch {
      // Not being able to remember the choice is not a reason to refuse
      // to apply it for this session.
    }
  })

  function cycle() {
    preference.value =
      preference.value === 'system' ? 'light'
      : preference.value === 'light' ? 'dark'
      : 'system'
  }

  return { preference, cycle }
}

/** Applied before Vue mounts so the first paint is already correct. */
export function applyStoredThemeEarly() {
  const p = readStored()
  if (p !== 'system') document.documentElement.setAttribute('data-theme', p)
}
