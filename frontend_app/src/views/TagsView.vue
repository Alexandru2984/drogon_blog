<script setup lang="ts">
import { ref, onMounted, computed } from 'vue'
import { postsApi, type TagSummary } from '@/api/posts'

const tags    = ref<TagSummary[]>([])
const loading = ref(true)
const error   = ref('')

onMounted(async () => {
  try {
    tags.value = await postsApi.listTags()
  } catch (e: any) {
    error.value = e?.response?.data?.error ?? 'Could not load tags'
  } finally {
    loading.value = false
  }
})

// Size each tag by how much is under it, the way a tag cloud is supposed to
// work — a flat list of forty identical chips tells the reader nothing about
// where the writing actually is. Bounded to four steps so the largest tag
// stays readable next to the smallest.
const maxCount = computed(() =>
  tags.value.reduce((m, t) => Math.max(m, t.count), 1))

function weight(count: number): number {
  const ratio = count / maxCount.value
  if (ratio > 0.75) return 4
  if (ratio > 0.5)  return 3
  if (ratio > 0.25) return 2
  return 1
}
</script>

<template>
  <h1 class="page-title">Tags</h1>

  <div v-if="loading" class="card" aria-hidden="true">
    <div class="skeleton line medium"></div>
    <div class="skeleton line"></div>
    <div class="skeleton line short"></div>
  </div>
  <p v-if="loading" class="visually-hidden" role="status">Loading tags…</p>

  <div v-else-if="error" class="empty-state" role="alert">
    <span class="emoji" aria-hidden="true">⚠️</span>
    <p class="error">{{ error }}</p>
  </div>

  <div v-else-if="!tags.length" class="empty-state">
    <span class="emoji" aria-hidden="true">🏷️</span>
    <p>No tags yet. They appear here as soon as a post uses one.</p>
  </div>

  <ul v-else class="cloud">
    <li v-for="t in tags" :key="t.slug">
      <router-link
        :to="{ name: 'tag', params: { slug: t.slug } }"
        class="cloud-tag"
        :class="`w${weight(t.count)}`"
      >
        {{ t.label }}
        <span class="cloud-count">{{ t.count }}</span>
      </router-link>
    </li>
  </ul>
</template>

<style scoped>
.cloud {
  display: flex;
  flex-wrap: wrap;
  gap: var(--sp-3);
  list-style: none;
  padding: 0;
  margin: 0;
}

.cloud-tag {
  display: inline-flex;
  align-items: baseline;
  gap: 0.45em;
  padding: 0.35em 0.9em;
  border-radius: var(--radius-pill);
  background: var(--bg-elev);
  border: 1px solid var(--border);
  color: var(--text);
  text-decoration: none;
  /* Comfortably past the 24 px AA target minimum at the smallest weight. */
  min-height: 2.25rem;
}
.cloud-tag:hover {
  border-color: var(--accent);
  color: var(--accent);
  text-decoration: none;
}

/* Weight by post count, not by importance to the author. */
.w1 { font-size: var(--step--1); }
.w2 { font-size: var(--step-0); }
.w3 { font-size: var(--step-1); font-weight: 550; }
.w4 { font-size: var(--step-2); font-weight: 600; }

.cloud-count {
  color: var(--text-faint);
  font-size: 0.72em;
  font-weight: 400;
  font-variant-numeric: tabular-nums;
}
</style>
