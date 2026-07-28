<script setup lang="ts">
// Placeholder shaped like PostCard, shown while the first page loads.
//
// A bare "Loading…" line is replaced by content of a completely different
// height, so the page jumps the moment data lands and anything the reader
// was about to click moves. Matching the real card's shape keeps the
// layout still.
defineProps<{ count?: number }>()
</script>

<template>
  <!-- aria-hidden: the skeleton carries no information, and a screen
       reader announcing three empty cards is noise. The loading state
       itself is announced by the region that owns it. -->
  <div aria-hidden="true">
    <article v-for="n in (count ?? 3)" :key="n" class="card post-card-skeleton">
      <div class="row tight">
        <span class="avatar sm skeleton"></span>
        <div style="flex: 1;">
          <div class="skeleton line short"></div>
          <div class="skeleton line" style="width: 30%;"></div>
        </div>
      </div>
      <div class="skeleton line" style="height: 1.4em; width: 65%;"></div>
      <div class="skeleton line"></div>
      <div class="skeleton line medium"></div>
    </article>
  </div>
</template>

<style scoped>
.post-card-skeleton { display: flex; flex-direction: column; gap: var(--sp-3); }
</style>
