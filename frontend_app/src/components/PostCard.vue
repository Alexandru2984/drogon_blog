<script setup lang="ts">
import type { Post } from '@/api/posts'
import { computed } from 'vue'

const props = defineProps<{ post: Post; clamp?: boolean }>()

const excerpt = computed(() => {
  if (!props.clamp) return props.post.content
  const max = 280
  return props.post.content.length > max
    ? props.post.content.slice(0, max).trimEnd() + '…'
    : props.post.content
})

function formatDate(s: string) {
  if (!s) return ''
  return new Date(s.replace(' ', 'T') + 'Z').toLocaleString()
}
</script>

<template>
  <article class="card">
    <header class="toolbar" style="margin-bottom: 0.5rem;">
      <span
        class="avatar"
        :style="post.author?.profile_image ? `background-image: url(${post.author.profile_image})` : ''"
      ></span>
      <div>
        <router-link
          v-if="post.author"
          :to="{ name: 'profile', params: { id: post.author.id } }"
          style="font-weight: 600;"
        >{{ post.author.username }}</router-link>
        <span v-else class="muted">unknown</span>
        <div class="muted" style="font-size: 0.8em;">{{ formatDate(post.created_at) }}</div>
      </div>
    </header>

    <router-link :to="{ name: 'post', params: { id: post.id } }" style="color: var(--text);">
      <h2 style="margin-bottom: 0.25rem;">{{ post.title }}</h2>
    </router-link>
    <p class="post-content">{{ excerpt }}</p>

    <div class="toolbar muted" style="margin-top: 0.75rem;">
      <router-link :to="{ name: 'post', params: { id: post.id } }">Read →</router-link>
    </div>
  </article>
</template>
