import { ref, watch } from 'vue'

const KEY = 'blog.reading.preferences.v1'

// Only presentation choices are stored. Article ids, bodies and reading
// history never enter localStorage through this feature.
export function useReadingPreferences() {
  const size = ref(1)
  const serif = ref(false)
  const measure = ref<'comfortable' | 'full'>('full')
  try {
    const saved = JSON.parse(localStorage.getItem(KEY) ?? 'null')
    if (saved && typeof saved === 'object') {
      if ([0.95, 1, 1.15, 1.3].includes(saved.size)) size.value = saved.size
      serif.value = saved.serif === true
      if (saved.measure === 'comfortable') measure.value = 'comfortable'
    }
  } catch { /* blocked storage or a malformed preference is harmless */ }
  watch([size, serif, measure], () => {
    try { localStorage.setItem(KEY, JSON.stringify({ size: size.value, serif: serif.value, measure: measure.value })) } catch { /* optional */ }
  })
  function reset() { size.value = 1; serif.value = false; measure.value = 'full' }
  return { size, serif, measure, reset }
}
