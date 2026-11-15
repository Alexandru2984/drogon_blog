<script setup lang="ts">
import Avatar from '@/components/Avatar.vue'
import { ref, computed, onMounted, watch } from 'vue'
import { usersApi } from '@/api/users'
import { authApi, type User } from '@/api/auth'
import { postsApi, type Post } from '@/api/posts'
import { useAuthStore } from '@/stores/auth'
import { useToastStore } from '@/stores/toast'
import PostCard from '@/components/PostCard.vue'
import PostCardSkeleton from '@/components/PostCardSkeleton.vue'
import FollowButton from '@/components/FollowButton.vue'
import ReportButton from '@/components/ReportButton.vue'
import { usePageMeta } from '@/composables/usePageMeta'

const props = defineProps<{ id: number }>()

const PAGE_SIZE = 20

const user = ref<User | null>(null)
const posts = ref<Post[]>([])
const nextCursor = ref<number | null>(null)
const loadingMore = ref(false)
const loading = ref(true)
const error = ref('')
const saving = ref(false)
const uploading = ref(false)
const resending = ref(false)
const verificationSent = ref(false)

const auth = useAuthStore()
const toasts = useToastStore()
const { setPageMeta } = usePageMeta()

watch(user, (value) => {
  if (!value) return
  setPageMeta({
    title: value.username,
    description: value.bio?.trim() || undefined,
  })
})

const isMe = computed(() => auth.isAuthed && auth.user!.id === props.id)
// GET /users/{id} deliberately omits email for privacy; for the owner we read
// the signed-in session's email so bio edits don't send an empty string.
const displayEmail = computed(() =>
  isMe.value ? (auth.user?.email ?? '') : (user.value?.email ?? ''),
)
const unverifiedBanner = computed(() =>
  isMe.value && auth.user?.email_verified === false,
)

const editing = ref(false)
const editBio = ref('')
const editEmail = ref('')
const editCurrentPassword = ref('')
const fileInput = ref<HTMLInputElement | null>(null)

const emailChanged = computed(
  () => isMe.value && editEmail.value.trim() !== (auth.user?.email ?? ''),
)

async function load() {
  loading.value = true
  error.value = ''
  editing.value = false
  nextCursor.value = null
  try {
    const [u, page] = await Promise.all([
      usersApi.get(props.id),
      postsApi.byUserPage(props.id, { limit: PAGE_SIZE }).catch(() => ({ posts: [], next_cursor: null })),
    ])
    user.value = u
    posts.value = page.posts
    nextCursor.value = page.next_cursor ?? null
    editBio.value = u.bio ?? ''
    editEmail.value = isMe.value ? (auth.user?.email ?? '') : ''
  } catch (e: any) {
    error.value = e?.response?.data?.error ?? 'User not found'
  } finally {
    loading.value = false
  }
}

async function loadMorePosts() {
  if (loadingMore.value || nextCursor.value == null) return
  loadingMore.value = true
  try {
    const page = await postsApi.byUserPage(props.id, { before: nextCursor.value, limit: PAGE_SIZE })
    const seen = new Set(posts.value.map(p => p.id))
    for (const p of page.posts) if (!seen.has(p.id)) posts.value.push(p)
    nextCursor.value = page.next_cursor ?? null
  } catch {
    toasts.push('Could not load more posts', 'error')
  } finally {
    loadingMore.value = false
  }
}

onMounted(load)
watch(() => props.id, load)

function startEditing() {
  editBio.value = user.value?.bio ?? ''
  editEmail.value = auth.user?.email ?? ''
  editCurrentPassword.value = ''
  editing.value = true
}

async function saveProfile() {
  saving.value = true
  try {
    const payload: { email?: string; bio?: string; current_password?: string } = {
      bio: editBio.value,
    }
    const wasEmailChange = emailChanged.value
    if (wasEmailChange) {
      payload.email = editEmail.value.trim()
      payload.current_password = editCurrentPassword.value
    }
    const res = await usersApi.updateProfile(payload)
    user.value = { ...user.value!, email: res.user.email, bio: res.user.bio }
    auth.patchUser({
      email: res.user.email,
      bio: res.user.bio,
      ...(wasEmailChange ? { email_verified: false } : {}),
    })
    editing.value = false
    editCurrentPassword.value = ''
    if (wasEmailChange) verificationSent.value = false
    toasts.push(
      wasEmailChange
        ? 'Profile updated. Check your email to verify the new address.'
        : 'Profile updated',
      'ok',
    )
  } catch (e: any) {
    toasts.push(e?.response?.data?.error ?? 'Could not save', 'error')
  } finally {
    saving.value = false
  }
}

async function onFileChange(ev: Event) {
  const target = ev.target as HTMLInputElement
  const file = target.files?.[0]
  if (!file) return
  uploading.value = true
  try {
    const res = await usersApi.uploadImage(file)
    user.value = { ...user.value!, profile_image: res.profile_image }
    auth.patchUser({ profile_image: res.profile_image })
    toasts.push('Avatar updated', 'ok')
  } catch (e: any) {
    toasts.push(e?.response?.data?.error ?? 'Upload failed', 'error')
  } finally {
    uploading.value = false
    if (fileInput.value) fileInput.value.value = ''
  }
}

async function resendVerification() {
  if (resending.value) return
  resending.value = true
  try {
    const res = await authApi.resendVerification()
    verificationSent.value = true
    toasts.push(res.message ?? 'Verification email sent', 'ok')
  } catch (e: any) {
    toasts.push(e?.response?.data?.error ?? 'Could not send verification email', 'error')
  } finally {
    resending.value = false
  }
}
</script>

<template>
  <template v-if="loading">
    <p class="visually-hidden" role="status">Loading profile…</p>
    <div class="card profile-head" aria-hidden="true">
      <span class="avatar lg skeleton"></span>
      <div style="flex: 1;">
        <div class="skeleton line" style="height: 1.5em; width: 45%;"></div>
        <div class="skeleton line short"></div>
      </div>
    </div>
    <PostCardSkeleton :count="2" />
  </template>

  <div v-else-if="error" class="empty-state" role="alert">
    <span class="emoji" aria-hidden="true">👤</span>
    <p class="error">{{ error }}</p>
    <router-link to="/" class="btn ghost">Back to the feed</router-link>
  </div>

  <template v-else-if="user">
    <header class="card profile-head">
      <Avatar size="lg" :image-url="user.profile_image" :username="user.username" />
      <div class="profile-ident">
        <h1 class="profile-name">
          {{ user.username }}
          <span v-if="user.online" class="online-pill" title="Online now">● Online</span>
        </h1>
        <div v-if="displayEmail" class="muted profile-email">{{ displayEmail }}</div>
      </div>
      <div class="row tight profile-head-actions">
        <button v-if="isMe && !editing" class="ghost" @click="startEditing">Edit profile</button>
        <router-link
          v-if="!isMe && auth.isAuthed"
          :to="{ name: 'messages', query: { user: id } }"
          class="btn ghost"
        >
          <span aria-hidden="true">✉️</span> Message
        </router-link>
      </div>
    </header>

    <div v-if="unverifiedBanner" class="card verify-banner" role="status">
      <div class="verify-banner-text">
        <strong>Email not verified</strong>
        <p class="muted" style="margin: 0;">
          Verify your email address to unlock every account feature.
        </p>
      </div>
      <button
        type="button"
        class="sm"
        :disabled="resending || verificationSent"
        @click="resendVerification"
      >
        {{ resending ? 'Sending…' : verificationSent ? 'Email sent ✓' : 'Resend verification email' }}
      </button>
    </div>

    <div class="card row profile-bar">
      <FollowButton :user-id="id" />
      <ReportButton v-if="!isMe && auth.isAuthed" target-type="user" :target-id="id" />
    </div>

    <section v-if="editing" class="card">
      <h2 class="section-heading">Edit profile</h2>
      <form @submit.prevent="saveProfile">
        <label for="prof-email">Email</label>
        <input id="prof-email" v-model="editEmail" type="email" autocomplete="email" />

        <template v-if="emailChanged">
          <label for="prof-cur-pw">Current password (required to change email)</label>
          <input id="prof-cur-pw" v-model="editCurrentPassword" type="password"
                 autocomplete="current-password" />
          <p class="muted" style="margin-top: var(--sp-2);">
            A verification email will be sent to the new address; access to the
            account is restricted until it's confirmed.
          </p>
        </template>

        <label for="prof-bio">Bio</label>
        <textarea id="prof-bio" v-model="editBio" rows="4" maxlength="500"></textarea>

        <label for="prof-avatar">Profile image</label>
        <input id="prof-avatar" ref="fileInput" type="file" accept="image/*"
               :disabled="uploading" @change="onFileChange" />
        <p class="muted" style="margin-top: var(--sp-2);">
          {{ uploading ? 'Uploading…' : 'JPEG / PNG / WebP. Uploaded immediately.' }}
        </p>

        <div class="row tight" style="margin-top: var(--sp-5);">
          <button type="submit" :disabled="saving || (emailChanged && !editCurrentPassword)">
            {{ saving ? 'Saving…' : 'Save' }}
          </button>
          <button type="button" class="ghost" @click="editing = false">Cancel</button>
        </div>
      </form>
    </section>

    <section v-else-if="user.bio" class="card">
      <p class="post-content" style="margin: 0;">{{ user.bio }}</p>
    </section>

    <h2 class="section-heading posts-heading">Posts</h2>
    <div v-if="!posts.length" class="empty-state">
      <span class="emoji" aria-hidden="true">📝</span>
      <p>{{ isMe ? "You haven't published anything yet." : 'No posts yet.' }}</p>
      <router-link v-if="isMe" :to="{ name: 'create-post' }" class="btn">Write your first post</router-link>
    </div>
    <PostCard v-for="p in posts" :key="p.id" :post="p" clamp />

    <div v-if="nextCursor != null" class="load-more-wrap">
      <button type="button" class="ghost" :disabled="loadingMore" @click="loadMorePosts">
        {{ loadingMore ? $t('common.loading') : $t('feed.load_more') }}
      </button>
    </div>
  </template>
</template>

<style scoped>
.profile-head {
  display: flex;
  flex-wrap: wrap;
  align-items: center;
  gap: var(--sp-4);
}
.profile-ident { flex: 1 1 10rem; min-width: 0; }
.profile-name {
  display: flex;
  flex-wrap: wrap;
  align-items: center;
  gap: var(--sp-2);
  font-size: var(--step-2);
  margin: 0 0 0.15em;
  overflow-wrap: anywhere;
}
.online-pill {
  font-size: 0.72rem;
  font-weight: 600;
  color: var(--ok);
  background: color-mix(in srgb, var(--ok) 12%, transparent);
  padding: 0.15em 0.55em;
  border-radius: 999px;
}
.profile-email { overflow-wrap: anywhere; }
.profile-head-actions { margin-left: auto; }

.verify-banner {
  display: flex;
  flex-wrap: wrap;
  align-items: center;
  justify-content: space-between;
  gap: var(--sp-3);
  border-color: var(--accent);
  background: color-mix(in srgb, var(--accent) 7%, var(--bg-elev));
}
.verify-banner-text { min-width: 0; flex: 1 1 14rem; }

.profile-bar {
  justify-content: space-between;
  align-items: center;
  flex-wrap: wrap;
}

.section-heading { font-size: var(--step-1); margin: 0 0 var(--sp-4); }
.posts-heading { margin-top: var(--sp-6); }
.load-more-wrap { display: flex; justify-content: center; margin-top: var(--sp-4); }
</style>
