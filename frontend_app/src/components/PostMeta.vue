<script setup lang="ts">
import { computed } from 'vue'

// The small "5 min read · 128 views" line under a post heading.
//
// Both numbers are optional on purpose: the feed sends them, older cached
// responses and the drafts list may not, and a card that renders "undefined
// min read" is worse than one that renders nothing.
const props = defineProps<{
  readingMinutes?: number
  viewCount?: number
  isDraft?: boolean
}>()

const parts = computed(() => {
  const out: string[] = []
  if (props.readingMinutes && props.readingMinutes > 0) {
    out.push(`${props.readingMinutes} min read`)
  }
  // A draft has been read by nobody; showing "0 views" on your own unfinished
  // note is just discouraging.
  if (!props.isDraft && typeof props.viewCount === 'number' && props.viewCount > 0) {
    out.push(`${props.viewCount} view${props.viewCount === 1 ? '' : 's'}`)
  }
  return out
})
</script>

<template>
  <p v-if="parts.length || isDraft" class="post-meta">
    <span v-if="isDraft" class="badge warn">Draft</span>
    <span v-for="(p, i) in parts" :key="p">
      <span v-if="i > 0 || isDraft" aria-hidden="true"> · </span>{{ p }}
    </span>
  </p>
</template>

<style scoped>
.post-meta {
  display: flex;
  flex-wrap: wrap;
  align-items: center;
  gap: 0 0.15em;
  margin: 0;
  color: var(--text-dim);
  font-size: var(--step--1);
}
.post-meta .badge { margin-right: 0.35em; }
</style>
