import { defineConfig } from 'vite'
import vue from '@vitejs/plugin-vue'
import { fileURLToPath, URL } from 'node:url'

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
        manualChunks: {
          vendor: ['vue', 'vue-router', 'pinia', 'axios'],
        },
      },
    },
  },
  server: {
    port: 5173,
    proxy: {
      // Backend API endpoints — proxy to Drogon during `npm run dev`.
      '/auth':     'http://127.0.0.1:8092',
      '/posts':    'http://127.0.0.1:8092',
      '/users':    'http://127.0.0.1:8092',
      '/comments': 'http://127.0.0.1:8092',
      '/messages': 'http://127.0.0.1:8092',
      '/uploads':  'http://127.0.0.1:8092',
      // WebSocket: needs ws:true so Vite proxies the Upgrade handshake.
      '/ws':       { target: 'ws://127.0.0.1:8092', ws: true },
    },
  },
})
