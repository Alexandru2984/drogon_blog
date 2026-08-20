import { defineConfig } from 'vite'
import vue from '@vitejs/plugin-vue'
import { fileURLToPath, URL } from 'node:url'

const backend = 'http://127.0.0.1:8092'

// Build output goes straight into ../public (Drogon's document_root).
// emptyOutDir is disabled so user-uploaded content under public/uploads/ is preserved.
export default defineConfig({
  plugins: [vue()],
  resolve: {
    alias: {
      '@': fileURLToPath(new URL('./src', import.meta.url)),
    },
  },
  build: {
    outDir: '../public',
    emptyOutDir: false,
    assetsDir: 'assets',
    sourcemap: false,
    rollupOptions: {
      output: {
        // Vite 8 + Rolldown require the function form of manualChunks (the
        // record form was dropped). Group runtime libraries into one chunk
        // so repeat visits get a warm cache for everything framework-side.
        manualChunks(id) {
          if (id.includes('/node_modules/vue/')       ||
              id.includes('/node_modules/vue-router/')||
              id.includes('/node_modules/@vue/')      ||
              id.includes('/node_modules/pinia/')     ||
              id.includes('/node_modules/axios/')) {
            return 'vendor'
          }
          return undefined
        },
      },
    },
  },
  server: {
    port: 5173,
    proxy: {
      // Axios sends every API request through this reserved prefix in dev.
      // Strip it at the proxy so Drogon still sees its production route.
      '/__api': {
        target: backend,
        rewrite: path => path.replace(/^\/__api/, ''),
      },
      // Upload URLs appear directly in img/src and Markdown, outside Axios.
      '/uploads': backend,
      // WebSocket: needs ws:true so Vite proxies the Upgrade handshake.
      '/ws':       { target: 'ws://127.0.0.1:8092', ws: true },
    },
  },
})
