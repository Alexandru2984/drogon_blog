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
function syncThemeColor(pref: ThemePreference) {
  if (typeof document === 'undefined') return
  const light = '#f7f8fa'
  const dark  = '#0d0f14'
  const metas = Array.from(document.querySelectorAll<HTMLMetaElement>('meta[name="theme-color"]'))
  if (!metas.length) {
    const m = document.createElement('meta')
    m.name = 'theme-color'
    document.head.appendChild(m)
    metas.push(m)
  }
  const sysDark = typeof window !== 'undefined' &&
    window.matchMedia?.('(prefers-color-scheme: dark)').matches
  for (const m of metas) {
    const media = m.getAttribute('media') || ''
    if (pref === 'light') {
      m.content = light
    } else if (pref === 'dark') {
      m.content = dark
    } else if (media.includes('dark')) {
      m.content = dark
    } else if (media.includes('light')) {
      m.content = light
    } else {
      m.content = sysDark ? dark : light
    }
  }
}

export function useTheme() {
  watchEffect(() => {
    const root = document.documentElement
    if (preference.value === 'system') root.removeAttribute('data-theme')
    else root.setAttribute('data-theme', preference.value)
    syncThemeColor(preference.value)

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
