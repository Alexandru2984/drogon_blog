// Flat-config ESLint for the Vue 3 + TypeScript SPA. Goal is signal-to-noise:
// no opinionated style enforcement (Prettier territory), just the lint rules
// that catch real defects.

import js          from '@eslint/js'
import ts          from 'typescript-eslint'
import vue         from 'eslint-plugin-vue'
import vueParser   from 'vue-eslint-parser'
import globals     from 'globals'

export default [
  js.configs.recommended,
  ...ts.configs.recommended,
  // `flat/essential` covers correctness rules without the formatting noise
  // of `flat/recommended` (max-attrs-per-line, html-self-closing, …).
  ...vue.configs['flat/essential'],

  {
    files: ['**/*.{ts,tsx,vue}'],
    languageOptions: {
      // SPA code runs in the browser; expose DOM + standard browser globals
      // so `IntersectionObserver`, `HTMLElement`, `setTimeout`, etc. resolve.
      globals: { ...globals.browser, ...globals.es2024 },
      ecmaVersion: 'latest',
      sourceType:  'module',
    },
  },

  {
    files: ['**/*.vue'],
    languageOptions: {
      parser: vueParser,
      parserOptions: {
        parser: ts.parser,
        ecmaVersion: 'latest',
        sourceType:  'module',
        extraFileExtensions: ['.vue'],
      },
    },
  },

  {
    rules: {
      // Component names like "PostCard" / "HomeView" are intentional and
      // unambiguous in this codebase.
      'vue/multi-word-component-names': 'off',
      // We rely on v-html only for server-rendered cmark-gfm output and
      // Postgres ts_headline; both are sanitised upstream.
      'vue/no-v-html': 'off',
      // Template shims-vue.d.ts declares `{}` as the props type; harmless.
      '@typescript-eslint/no-empty-object-type': 'off',
      // axios error catches are typed `unknown` from TS, then narrowed.
      '@typescript-eslint/no-explicit-any':      'off',
      '@typescript-eslint/no-unused-vars': ['error', {
        argsIgnorePattern:              '^_',
        destructuredArrayIgnorePattern: '^_',
        caughtErrorsIgnorePattern:      '^_',
      }],
    },
  },

  { ignores: ['dist/**', 'node_modules/**'] },
]
