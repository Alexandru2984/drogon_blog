<script setup lang="ts">
import type { Tag } from '@/api/posts'

// Tags render as links, not as decoration: a tag the reader cannot click is
// a label, and a label does not justify the visual weight a chip carries.
defineProps<{ tags?: Tag[]; small?: boolean }>()
</script>

<template>
  <ul v-if="tags && tags.length" class="tag-list" :class="{ small }">
    <li v-for="t in tags" :key="t.slug">
      <router-link :to="{ name: 'tag', params: { slug: t.slug } }" class="tag">
        {{ t.label }}
      </router-link>
    </li>
  </ul>
</template>

<style scoped>
/* A list, so assistive technology announces how many tags there are rather
   than reading a run of unrelated links. */
.tag-list {
  display: flex;
  flex-wrap: wrap;
  gap: var(--sp-2);
  list-style: none;
  padding: 0;
  margin: 0;
}

.tag {
  display: inline-flex;
  align-items: center;
  padding: 0.2em 0.7em;
  border-radius: var(--radius-pill);
  background: var(--bg-inset);
  border: 1px solid transparent;
  color: var(--text-dim);
  font-size: var(--step--1);
  font-weight: 550;
  line-height: 1.6;
  /* A chip is its own block, so it is exempt from the underline rule that
     applies to links inside prose. */
  text-decoration: none;
}
.tag:hover {
  background: var(--accent-soft);
  border-color: var(--accent);
  color: var(--accent);
  text-decoration: none;
}

.tag-list.small .tag { font-size: 0.72rem; padding: 0.1em 0.55em; }
</style>
