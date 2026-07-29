<script setup lang="ts">
import type { Post } from '@/api/posts'
import { computed } from 'vue'
import TagList from '@/components/TagList.vue'
import PostMeta from '@/components/PostMeta.vue'

const props = defineProps<{ post: Post; clamp?: boolean }>()

// Prefer the server-built excerpt: it has the markdown syntax stripped, so
// a card no longer shows literal "## " or "```" where a heading or a code
// fence happened to fall inside the first 280 characters. Falls back to the
// raw content for rows written before the excerpt column existed.
const excerpt = computed(() => {
  if (props.post.excerpt) return props.post.excerpt
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

// Machine-readable timestamp for <time datetime>. The displayed string is
// locale-formatted and useless to a parser or an assistive technology
// trying to say "three days ago".
const isoDate = computed(() => {
  const d = new Date(props.post.created_at.replace(' ', 'T') + 'Z')
  return isNaN(d.getTime()) ? '' : d.toISOString()
})
</script>

<template>
  <article class="card post-card">
    <header class="row tight">
      <span
        class="avatar sm"
        :style="post.author?.profile_image ? `background-image: url(${post.author.profile_image})` : ''"
        aria-hidden="true"
      ></span>
      <div class="post-card-meta">
        <router-link
          v-if="post.author"
          :to="{ name: 'profile', params: { id: post.author.id } }"
          class="post-card-author meta-link"
        >{{ post.author.username }}</router-link>
        <span v-else class="muted">unknown</span>
        <time v-if="isoDate" :datetime="isoDate" class="muted">
          {{ formatDate(post.created_at) }}
        </time>
      </div>
    </header>

    <h2 class="post-card-title">
      <router-link :to="{ name: 'post', params: { id: post.id } }">
        {{ post.title }}
      </router-link>
    </h2>

    <PostMeta :reading-minutes="post.reading_minutes" :view-count="post.view_count" />

    <p class="post-content post-card-excerpt">{{ excerpt }}</p>

    <TagList :tags="post.tags" small />

    <router-link :to="{ name: 'post', params: { id: post.id } }" class="link-action post-card-more">
      Read <span aria-hidden="true">→</span>
    </router-link>
  </article>
</template>

<style scoped>
.post-card { display: flex; flex-direction: column; gap: var(--sp-3); }

.post-card-meta {
  display: flex;
  flex-direction: column;
  line-height: 1.35;
  min-width: 0;
}
.post-card-author { font-weight: 600; }
.post-card-meta time { font-size: 0.78rem; }

/* A feed card is a summary, not the article. The global h2 scale runs up to
   2.15rem, which reads as a page heading and made every card shout. This
   keeps the semantic level — it is still the card's heading — while sizing
   it as one item in a list. */
.post-card-title {
  margin: 0;
  font-size: var(--step-1);
  line-height: 1.3;
}
.post-card-title a { color: var(--text); }
.post-card-title a:hover { color: var(--accent); text-decoration: none; }

.post-card-excerpt {
  margin: 0;
  color: var(--text-dim);
  /* Cap the preview at four lines so one long post cannot dominate the
     feed. The 280-character clamp above still applies; this covers the
     case where those characters happen to be many short lines. */
  display: -webkit-box;
  -webkit-line-clamp: 4;
  line-clamp: 4;
  -webkit-box-orient: vertical;
  overflow: hidden;
}

.post-card-more { font-size: var(--step--1); align-self: start; }
</style>
