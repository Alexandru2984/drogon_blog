<script setup lang="ts">
import { formatDateTime, isoDate as toIsoDate } from '@/lib/date'
import { onMounted, computed } from 'vue'
import { storeToRefs } from 'pinia'
import { useNotificationsStore } from '@/stores/notifications'
import type { AppNotification } from '@/api/social'
import Avatar from '@/components/Avatar.vue'

const store = useNotificationsStore()
const { items, unread, loading, loadingMore, nextCursor } = storeToRefs(store)

onMounted(() => store.load())

const hasUnread = computed(() => unread.value > 0)

function summary(n: AppNotification): string {
  const who = n.actor?.username ?? 'Someone'
  switch (n.kind) {
    case 'comment':  return `${who} commented on your post`
    case 'reply':    return `${who} replied to your comment`
    case 'follow':   return `${who} followed you`
    case 'new_post': return `${who} published a new post`
    case 'like':     return `${who} liked your post`
    default:         return `${who} did something`
  }
}

function icon(kind: AppNotification['kind']): string {
  switch (kind) {
    case 'comment':  return '💬'
    case 'reply':    return '↩️'
    case 'follow':   return '👤'
    case 'new_post': return '📝'
    case 'like':     return '♥'
    default:         return '•'
  }
}

function target(n: AppNotification) {
  if (n.kind === 'follow' && n.actor) {
    return { name: 'profile', params: { id: n.actor.id } }
  }
  if (n.post_id) {
    const hash = (n.kind === 'comment' || n.kind === 'reply') && n.comment_id
      ? `#comment-${n.comment_id}`
      : undefined
    return { name: 'post', params: { id: n.post_id }, ...(hash ? { hash } : {}) }
  }
  return { name: 'home' }
}

function when(s: string) { return formatDateTime(s) }
function iso(s: string) { return toIsoDate(s) }
</script>

<template>
  <header class="row tight notif-head">
    <h1 class="page-title" style="margin: 0;">{{ $t('nav.notifications') }}</h1>
    <span class="spacer"></span>
    <button v-if="hasUnread" class="ghost sm" @click="store.markAllRead()">
      Mark all read
    </button>
  </header>

  <template v-if="loading && !items.length">
    <p class="visually-hidden" role="status">Loading notifications…</p>
    <div v-for="n in 3" :key="n" class="card notif" aria-hidden="true">
      <span class="avatar sm skeleton"></span>
      <div style="flex: 1;">
        <div class="skeleton line medium"></div>
        <div class="skeleton line short"></div>
      </div>
    </div>
  </template>

  <div v-else-if="!items.length" class="empty-state">
    <span class="emoji" aria-hidden="true">🔔</span>
    <p>Nothing yet. Replies, follows and likes show up here.</p>
  </div>

  <template v-else>
    <ul class="notif-list">
      <li v-for="n in items" :key="n.id">
        <router-link
          :to="target(n)"
          class="card notif"
          :class="{ unread: !n.read }"
          @click="store.markRead(n.id)"
        >
          <span class="notif-avatar-wrap">
            <Avatar size="sm" :image-url="n.actor?.profile_image" :username="n.actor?.username" />
            <span class="notif-icon-badge" aria-hidden="true">{{ icon(n.kind) }}</span>
          </span>
          <span class="notif-body">
            <span class="notif-summary">
              {{ summary(n) }}
              <span v-if="!n.read" class="notif-dot" aria-hidden="true"></span>
              <span v-if="!n.read" class="visually-hidden">(unread)</span>
            </span>
            <span v-if="n.post_title" class="notif-target">{{ n.post_title }}</span>
            <span v-if="n.comment_preview" class="notif-preview">{{ n.comment_preview }}</span>
            <time v-if="iso(n.created_at)" :datetime="iso(n.created_at)" class="notif-when">
              {{ when(n.created_at) }}
            </time>
          </span>
        </router-link>
      </li>
    </ul>

    <div v-if="nextCursor != null" class="load-more-wrap">
      <button type="button" class="ghost" :disabled="loadingMore" @click="store.loadMore()">
        {{ loadingMore ? $t('common.loading') : $t('feed.load_more') }}
      </button>
    </div>
  </template>
</template>

<style scoped>
.notif-head { margin-bottom: var(--sp-5); }

.notif-list { list-style: none; padding: 0; margin: 0; }
.notif-list li + li { margin-top: var(--sp-3); }

.notif {
  display: flex;
  gap: var(--sp-3);
  align-items: flex-start;
  color: var(--text);
  text-decoration: none;
}
.notif:hover { border-color: var(--accent); text-decoration: none; }
.notif.unread { border-left: 3px solid var(--accent); }

.notif-avatar-wrap {
  position: relative;
  display: inline-flex;
  flex: 0 0 auto;
  margin-top: 0.1rem;
}
.notif-icon-badge {
  position: absolute;
  right: -0.35rem;
  bottom: -0.25rem;
  font-size: 0.72rem;
  line-height: 1;
  background: var(--bg-elev);
  border: 1px solid var(--border-soft);
  border-radius: 999px;
  padding: 0.12rem;
}

.notif-body { display: flex; flex-direction: column; gap: 0.15em; min-width: 0; }
.notif-summary { font-weight: 550; }

.notif-dot {
  display: inline-block;
  width: 0.5em; height: 0.5em;
  margin-left: 0.4em;
  border-radius: 50%;
  background: var(--accent);
  vertical-align: middle;
}

.notif-target {
  color: var(--text-dim);
  font-size: var(--step--1);
  overflow-wrap: anywhere;
}
.notif-preview {
  color: var(--text-dim);
  font-size: var(--step--1);
  font-style: italic;
  overflow-wrap: anywhere;
}
.notif-when { color: var(--text-faint); font-size: 0.72rem; }
.load-more-wrap { display: flex; justify-content: center; margin-top: var(--sp-4); }
</style>
