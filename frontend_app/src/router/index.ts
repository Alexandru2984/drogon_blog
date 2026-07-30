import { createRouter, createWebHashHistory, type RouteRecordRaw } from 'vue-router'
import { useAuthStore } from '@/stores/auth'

const routes: RouteRecordRaw[] = [
  { path: '/',                  name: 'home',          component: () => import('@/views/HomeView.vue') },
  { path: '/login',             name: 'login',         component: () => import('@/views/LoginView.vue') },
  { path: '/register',          name: 'register',      component: () => import('@/views/RegisterView.vue') },
  { path: '/forgot-password',   name: 'forgot',        component: () => import('@/views/ForgotPasswordView.vue') },
  { path: '/reset-password',    name: 'reset',         component: () => import('@/views/ResetPasswordView.vue') },
  { path: '/verify-email',      name: 'verify',        component: () => import('@/views/VerifyEmailView.vue') },
  // `wide` widens the main container to --wide-width: the editor puts the
  // markdown and its preview side by side above 900 px, and two panes do not
  // fit in the 44rem reading measure the rest of the app uses.
  { path: '/posts/new',         name: 'create-post',   component: () => import('@/views/CreatePostView.vue'), meta: { auth: true, wide: true } },
  { path: '/search',            name: 'search',        component: () => import('@/views/SearchView.vue') },
  { path: '/tags',              name: 'tags',          component: () => import('@/views/TagsView.vue') },
  { path: '/tags/:slug',        name: 'tag',           component: () => import('@/views/TagView.vue'), props: true },
  // Drafts are the author's own; the auth guard is what keeps the route
  // from rendering an empty list to a signed-out visitor.
  { path: '/drafts',            name: 'drafts',        component: () => import('@/views/DraftsView.vue'), meta: { auth: true } },
  { path: '/bookmarks',         name: 'bookmarks',     component: () => import('@/views/BookmarksView.vue'), meta: { auth: true } },
  { path: '/notifications',     name: 'notifications', component: () => import('@/views/NotificationsView.vue'), meta: { auth: true } },
  { path: '/messages',          name: 'messages',      component: () => import('@/views/MessagesView.vue'), meta: { auth: true } },
  { path: '/posts/:id',         name: 'post',          component: () => import('@/views/PostView.vue'), props: r => ({ id: Number(r.params.id) }) },
  { path: '/profile/:id',       name: 'profile',       component: () => import('@/views/ProfileView.vue'), props: r => ({ id: Number(r.params.id) }) },
  { path: '/login/2fa',         name: 'verify-2fa',    component: () => import('@/views/Verify2FAView.vue') },
  { path: '/account/security',  name: 'security-2fa',  component: () => import('@/views/Security2FAView.vue'), meta: { auth: true } },
  // A real 404 rather than a redirect to '/'. Silently landing a bad link on
  // the feed makes a broken URL indistinguishable from a working one.
  { path: '/:catchAll(.*)',     name: 'not-found',     component: () => import('@/views/NotFoundView.vue') },
]

export const router = createRouter({
  history: createWebHashHistory(),
  routes,
  scrollBehavior() { return { top: 0 } },
})

// Wait for the auth store's first /auth/me probe to land before the guard
// decides. main.ts awaits fetchMe via .finally(() => app.mount()), so in
// the happy path `ready` is already true when the guard runs — this is
// just defensive for the rare path where the initial navigation fires
// before fetchMe's microtask completes (observed on page.reload() into a
// `meta.auth` route during e2e). Without it, the guard reads
// `isAuthed=false`, bounces to /login, and the SPA gets stuck even though
// /auth/me would have come back 200.
router.beforeEach(async (to) => {
  if (!to.meta?.auth) return
  const auth = useAuthStore()
  if (!auth.ready) {
    await auth.fetchMe()
  }
  if (!auth.isAuthed) return { name: 'login', query: { next: to.fullPath } }
})
