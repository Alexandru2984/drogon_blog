<script setup lang="ts">
import { ref, computed, onMounted, watch } from 'vue'
import { usersApi } from '@/api/users'
import { postsApi, type Post } from '@/api/posts'
import { useAuthStore } from '@/stores/auth'
import { useToastStore } from '@/stores/toast'
import type { User } from '@/api/auth'
import PostCard from '@/components/PostCard.vue'

const props = defineProps<{ id: number }>()

const user = ref<User | null>(null)
const posts = ref<Post[]>([])
const loading = ref(true)
const error = ref('')

const auth = useAuthStore()
const toasts = useToastStore()

const isMe = computed(() => auth.isAuthed && auth.user!.id === props.id)
const editing = ref(false)
const editBio = ref('')
const editEmail = ref('')
const fileInput = ref<HTMLInputElement | null>(null)

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

async function saveProfile() {
  try {
    const res = await usersApi.updateProfile({ email: editEmail.value, bio: editBio.value })
    user.value = { ...user.value!, email: res.user.email, bio: res.user.bio }
    auth.patchUser({ email: res.user.email, bio: res.user.bio })
    editing.value = false
    toasts.push('Profile updated', 'ok')
  } catch (e: any) {
    toasts.push(e?.response?.data?.error ?? 'Could not save', 'error')
  }
}

async function onFileChange(ev: Event) {
  const target = ev.target as HTMLInputElement
  const file = target.files?.[0]
  if (!file) return
  try {
    const res = await usersApi.uploadImage(file)
    user.value = { ...user.value!, profile_image: res.profile_image }
    auth.patchUser({ profile_image: res.profile_image })
    toasts.push('Avatar updated', 'ok')
  } catch (e: any) {
    toasts.push(e?.response?.data?.error ?? 'Upload failed', 'error')
  } finally {
    if (fileInput.value) fileInput.value.value = ''
  }
}
</script>

<template>
  <p v-if="loading" class="muted">Loading…</p>
  <p v-else-if="error" class="error">{{ error }}</p>

  <template v-else-if="user">
    <header class="card toolbar">
      <span
        class="avatar lg"
        :style="user.profile_image ? `background-image: url(${user.profile_image})` : ''"
      ></span>
      <div>
        <h2 style="margin-bottom: 0.25rem;">{{ user.username }}</h2>
        <div class="muted">{{ user.email }}</div>
      </div>
      <span class="spacer"></span>
      <button v-if="isMe && !editing" class="ghost" @click="editing = true">Edit profile</button>
    </header>

    <section v-if="editing" class="card">
      <label>Email</label>
      <input v-model="editEmail" type="email" />
      <label>Bio</label>
      <textarea v-model="editBio" rows="4"></textarea>
      <label>Profile image</label>
      <input ref="fileInput" type="file" accept="image/*" @change="onFileChange" />
      <div class="toolbar" style="margin-top: 1rem;">
        <button @click="saveProfile">Save</button>
        <button class="ghost" @click="editing = false">Cancel</button>
      </div>
    </section>

    <section v-else-if="user.bio" class="card">
      <p style="margin: 0;">{{ user.bio }}</p>
    </section>

    <h3 style="margin-top: 1.5rem;">Posts</h3>
    <p v-if="!posts.length" class="muted">No posts yet.</p>
    <PostCard v-for="p in posts" :key="p.id" :post="p" clamp />
  </template>
</template>
