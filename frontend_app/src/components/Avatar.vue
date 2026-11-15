<script setup lang="ts">
import { computed } from 'vue'

const props = defineProps<{
  imageUrl?: string | null
  username?: string | null
  size?: 'sm' | 'md' | 'lg'
}>()

const safeBgStyle = computed(() => {
  const raw = props.imageUrl?.trim()
  if (!raw) return ''
  // Allow only same-origin root-relative paths or http(s) URLs.
  if (!raw.startsWith('/') && !/^https?:\/\//i.test(raw)) return ''
  // Escape characters that could break out of CSS url("...") context.
  const escaped = raw.replace(/[\r\n\0\\"'()]/g, (ch) => `\\${ch.charCodeAt(0).toString(16)} `)
  return `background-image: url("${escaped}")`
})

const initial = computed(() => {
  if (safeBgStyle.value) return ''
  const name = props.username?.trim()
  if (!name) return ''
  return name.charAt(0).toUpperCase()
})
</script>

<template>
  <span
    class="avatar"
    :class="size === 'lg' ? 'lg' : size === 'sm' ? 'sm' : ''"
    :style="safeBgStyle"
    aria-hidden="true"
  >{{ initial }}</span>
</template>

<style scoped>
.avatar {
  display: inline-flex;
  align-items: center;
  justify-content: center;
  font-size: 0.78rem;
  font-weight: 700;
  color: var(--text-muted);
  user-select: none;
}
.avatar.sm { font-size: 0.68rem; }
.avatar.lg { font-size: 1.45rem; }
</style>
