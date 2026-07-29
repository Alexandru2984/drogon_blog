<script setup lang="ts">
import { useRoute } from 'vue-router'

// The router previously mapped every unmatched path to a redirect to '/'.
// That turns a typo, a stale bookmark or a broken inbound link into a silent
// landing on the feed: the reader is left thinking the post they clicked is
// what they are looking at, and there is nothing to report as broken.
const route = useRoute()
</script>

<template>
  <div class="empty-state notfound">
    <span class="emoji" aria-hidden="true">🧭</span>
    <h1 class="notfound-title">Nothing here</h1>
    <p>
      <code class="notfound-path">{{ route.fullPath }}</code> doesn't match
      anything on this blog. It may have been removed, or the link may be
      incomplete.
    </p>
    <div class="row tight notfound-actions">
      <router-link to="/" class="btn">Back to the feed</router-link>
      <router-link :to="{ name: 'search' }" class="btn ghost">Search posts</router-link>
    </div>
  </div>
</template>

<style scoped>
.notfound { padding-block: var(--sp-7); }
.notfound-title { font-size: var(--step-2); margin: 0 0 var(--sp-3); color: var(--text); }
.notfound-path {
  font-family: var(--font-mono);
  font-size: 0.9em;
  overflow-wrap: anywhere;
  background: var(--bg-inset);
  padding: 0.15em 0.4em;
  border-radius: var(--radius-sm);
}
.notfound-actions { justify-content: center; margin-top: var(--sp-5); }
</style>
