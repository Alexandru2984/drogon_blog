import { createApp } from 'vue'
import { createPinia } from 'pinia'
import App from './App.vue'
import { router } from './router'
import { useAuthStore } from './stores/auth'
import './style.css'

const app = createApp(App)
app.use(createPinia())
app.use(router)

// Resolve current session before first render so auth-aware UI doesn't flicker.
const auth = useAuthStore()
auth.fetchMe().finally(() => app.mount('#app'))
