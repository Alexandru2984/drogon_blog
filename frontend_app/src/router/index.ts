import { createRouter, createWebHashHistory, type RouteRecordRaw } from 'vue-router'
import { useAuthStore } from '@/stores/auth'

const routes: RouteRecordRaw[] = [
  { path: '/',                  name: 'home',          component: () => import('@/views/HomeView.vue') },
  { path: '/login',             name: 'login',         component: () => import('@/views/LoginView.vue') },
  { path: '/register',          name: 'register',      component: () => import('@/views/RegisterView.vue') },
  { path: '/forgot-password',   name: 'forgot',        component: () => import('@/views/ForgotPasswordView.vue') },
  { path: '/reset-password',    name: 'reset',         component: () => import('@/views/ResetPasswordView.vue') },
  { path: '/verify-email',      name: 'verify',        component: () => import('@/views/VerifyEmailView.vue') },
  { path: '/posts/new',         name: 'create-post',   component: () => import('@/views/CreatePostView.vue'), meta: { auth: true } },
  { path: '/posts/:id',         name: 'post',          component: () => import('@/views/PostView.vue'), props: r => ({ id: Number(r.params.id) }) },
  { path: '/profile/:id',       name: 'profile',       component: () => import('@/views/ProfileView.vue'), props: r => ({ id: Number(r.params.id) }) },
  { path: '/:catchAll(.*)',     redirect: '/' },
]

export const router = createRouter({
  history: createWebHashHistory(),
  routes,
  scrollBehavior() { return { top: 0 } },
})

router.beforeEach((to) => {
  if (to.meta?.auth) {
    const auth = useAuthStore()
    if (!auth.isAuthed) return { name: 'login', query: { next: to.fullPath } }
  }
})
