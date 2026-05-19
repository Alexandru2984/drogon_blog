import { defineStore } from 'pinia'
import { ref } from 'vue'

interface Toast { id: number; text: string; kind: 'ok' | 'error' | 'info' }

let seq = 0

export const useToastStore = defineStore('toast', () => {
  const items = ref<Toast[]>([])

  function push(text: string, kind: Toast['kind'] = 'info') {
    const t = { id: ++seq, text, kind }
    items.value.push(t)
    setTimeout(() => {
      items.value = items.value.filter(i => i.id !== t.id)
    }, 3500)
  }

  return { items, push }
})
