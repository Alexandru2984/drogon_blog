<script setup lang="ts">
import { ref, watch } from 'vue'
import { postsApi, type RelatedPost } from '@/api/posts'
import PostMeta from '@/components/PostMeta.vue'
import TagList from '@/components/TagList.vue'

// "More like this", built from tags the two posts actually share.
//
// Deliberately not a "newest posts" fallback when nothing matches: a reader
// who finished an article on Postgres indexes is being told these are
// related, and filling the slot with whatever was published yesterday makes
// that claim false. No overlap means the section is not rendered at all.

const props = defineProps<{ postId: number }>()

const posts = ref<RelatedPost[]>([])
const loading = ref(true)

async function load(id: number) {
  loading.value = true
  try {
    posts.value = await postsApi.related(id)
  } catch {
    // Suggestions are an extra. A failure here must not put an error banner
    // on an article that loaded perfectly well.
    posts.value = []
  } finally {
    loading.value = false
  }
}

watch(() => props.postId, (id) => { load(id) }, { immediate: true })
</script>

<template>
  <section v-if="!loading && posts.length" class="related" aria-labelledby="related-heading">
    <h2 id="related-heading" class="related-heading">More like this</h2>
    <ul class="related-list">
      <li v-for="p in posts" :key="p.id" class="related-item card">
        <router-link :to="{ name: 'post', params: { id: p.id } }" class="related-title">
          {{ p.title }}
        </router-link>
        <p v-if="p.excerpt" class="related-excerpt">{{ p.excerpt }}</p>
        <div class="related-foot">
          <PostMeta :reading-minutes="p.reading_minutes" :view-count="p.view_count" />
          <TagList :tags="p.tags" small />
        </div>
      </li>
    </ul>
  </section>
</template>

<style scoped>
.related { margin-top: var(--sp-6); }

.related-heading {
  font-size: var(--step-1);
  margin: 0 0 var(--sp-4);
}

.related-list {
  margin: 0;
  padding: 0;
  list-style: none;
  display: grid;
  gap: var(--sp-3);
}
/* Two columns once there is room for them; one card per row on a phone,
   which is where most of these get tapped. */
@media (min-width: 700px) {
  .related-list { grid-template-columns: repeat(2, minmax(0, 1fr)); }
}

.related-item {
  display: flex;
  flex-direction: column;
  gap: var(--sp-2);
  margin: 0;
}

.related-title {
  font-weight: 600;
  font-size: var(--step-0);
  color: var(--text);
  overflow-wrap: anywhere;
}
.related-title:hover { color: var(--accent); }

.related-excerpt {
  margin: 0;
  color: var(--text-dim);
  font-size: var(--step--1);
  display: -webkit-box;
  -webkit-line-clamp: 2;
  line-clamp: 2;
  -webkit-box-orient: vertical;
  overflow: hidden;
}

.related-foot {
  margin-top: auto;
  display: flex;
  flex-direction: column;
  gap: var(--sp-2);
}
</style>
