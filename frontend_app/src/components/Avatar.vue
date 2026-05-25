<script setup lang="ts">
// Profile avatar bubble used across the app. When `imageUrl` is empty
// the avatar falls back to the global .avatar background (theme-set).
// Keeping the markup in one place means a future "show initials"
// fallback only ships once.

defineProps<{ imageUrl?: string }>()
</script>

<template>
  <span
    class="avatar"
    :style="imageUrl ? `background-image: url(${imageUrl})` : ''"
  ></span>
</template>
