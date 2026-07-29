<script setup lang="ts">
import { ref, computed, onMounted, watch } from 'vue'
import { usersApi } from '@/api/users'
import { postsApi, type Post } from '@/api/posts'
import { useAuthStore } from '@/stores/auth'
import { useToastStore } from '@/stores/toast'
import type { User } from '@/api/auth'
import PostCard from '@/components/PostCard.vue'
import PostCardSkeleton from '@/components/PostCardSkeleton.vue'
import FollowButton from '@/components/FollowButton.vue'

const props = defineProps<{ id: number }>()

const user = ref<User | null>(null)
const posts = ref<Post[]>([])
const loading = ref(true)
const error = ref('')
const saving = ref(false)
const uploading = ref(false)

const auth = useAuthStore()
const toasts = useToastStore()

const isMe = computed(() => auth.isAuthed && auth.user!.id === props.id)
const editing = ref(false)
const editBio = ref('')
const editEmail = ref('')
const editCurrentPassword = ref('')
const fileInput = ref<HTMLInputElement | null>(null)

const emailChanged = computed(
  () => !!user.value && editEmail.value !== (user.value.email ?? '')
)

async function load() {
  loading.value = true
  error.value = ''
  try {
    const [u, ps] = await Promise.all([
      usersApi.get(props.id),
      postsApi.byUser(props.id).catch(() => []),
    ])
    user.value = u
    posts.value = ps
    editBio.value = u.bio ?? ''
    editEmail.value = u.email ?? ''
  } catch (e: any) {
    error.value = e?.response?.data?.error ?? 'User not found'
  } finally {
    loading.value = false
  }
}

onMounted(load)
watch(() => props.id, load)

function startEditing() {
  // Reset from the loaded user rather than keeping whatever a previous
  // cancelled edit left behind.
  editBio.value = user.value?.bio ?? ''
  editEmail.value = user.value?.email ?? ''
  editCurrentPassword.value = ''
  editing.value = true
}

async function saveProfile() {
  saving.value = true
  try {
    const payload: { email?: string; bio?: string; current_password?: string } = {
      email: editEmail.value,
      bio:   editBio.value,
    }
    const wasEmailChange = emailChanged.value
    if (wasEmailChange) payload.current_password = editCurrentPassword.value
    const res = await usersApi.updateProfile(payload)
    user.value = { ...user.value!, email: res.user.email, bio: res.user.bio }
    auth.patchUser({ email: res.user.email, bio: res.user.bio })
    editing.value = false
    editCurrentPassword.value = ''
    toasts.push(
      wasEmailChange
        ? 'Profile updated. Check your email to verify the new address.'
        : 'Profile updated',
      'ok')
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
      <span
        class="avatar lg"
        :style="user.profile_image ? `background-image: url(${user.profile_image})` : ''"
        aria-hidden="true"
      ></span>
      <div class="profile-ident">
        <h1 class="profile-name">{{ user.username }}</h1>
        <div v-if="user.email" class="muted profile-email">{{ user.email }}</div>
      </div>
      <button v-if="isMe && !editing" class="ghost" @click="startEditing">Edit profile</button>
    </header>

    <div class="card">
      <FollowButton :user-id="id" />
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
  </template>
</template>

<style scoped>
/* Not .row: at 320 px the avatar, the name and the Edit button competed for
   one line and the name was squeezed to a couple of characters. The button
   drops to its own row instead. */
.profile-head {
  display: flex;
  flex-wrap: wrap;
  align-items: center;
  gap: var(--sp-4);
}
.profile-ident { flex: 1 1 10rem; min-width: 0; }
.profile-name {
  font-size: var(--step-2);
  margin: 0 0 0.15em;
  overflow-wrap: anywhere;
}
/* An address long enough to have no spaces in it still has to fit. */
.profile-email { overflow-wrap: anywhere; }

.section-heading { font-size: var(--step-1); margin: 0 0 var(--sp-4); }
.posts-heading { margin-top: var(--sp-6); }
</style>
