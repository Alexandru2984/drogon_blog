/// <reference types="vite/client" />

// Augment ImportMetaEnv with our own VITE_* keys so import.meta.env.X
// typechecks. Pull from .env / .env.local / env passed to the build.
interface ImportMetaEnv {
  readonly VITE_SENTRY_DSN?: string
}

interface ImportMeta {
  readonly env: ImportMetaEnv
}
