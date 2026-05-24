declare module '*.vue' {
  import type { DefineComponent } from 'vue'
  const component: DefineComponent<{}, {}, any>
  export default component
}

// TS 6 enforces module type declarations for side-effect imports; Vite handles
// `*.css` at bundle time but vue-tsc still wants an ambient module shim.
declare module '*.css'
