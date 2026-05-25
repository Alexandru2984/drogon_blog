import { createApp } from 'vue'
import { createPinia } from 'pinia'
import * as Sentry from '@sentry/vue'
import App from './App.vue'
import { router } from './router'
import { useAuthStore } from './stores/auth'
import './style.css'

const app = createApp(App)

// Sentry — opt-in via VITE_SENTRY_DSN at build time. Mirrors the
// backend's BLOG_SENTRY_DSN env. When unset, Sentry.init is skipped
// and the bundle ships with the SDK present but inert (the tree-
// shaken minified bundle keeps the unused SDK code out of the
// critical path). The browser-router integration captures route
// names alongside the error trace.
const sentryDsn = import.meta.env.VITE_SENTRY_DSN
if (sentryDsn) {
  Sentry.init({
    app,
    dsn:                sentryDsn,
    environment:        import.meta.env.MODE,
    // Errors-only by default. Bump tracesSampleRate later when we
    // care about UX performance signals.
    tracesSampleRate:   0,
    // Hide PII unless explicitly needed (Sentry's defaults are
    // mostly safe but the explicit knob makes the choice visible).
    sendDefaultPii:     false,
    integrations:       [Sentry.browserTracingIntegration({ router })],
  })
}

app.use(createPinia())
app.use(router)

// Resolve current session before first render so auth-aware UI doesn't flicker.
const auth = useAuthStore()
auth.fetchMe().finally(() => app.mount('#app'))
