import type { StorybookConfig } from '@storybook/vue3-vite'

// Storybook config for the Vue 3 + Vite stack. Stories live next to
// the components they cover (src/components/*.stories.ts), which keeps
// drift to a minimum — a story can't get separated from its component
// by an accidental move.
//
// We deliberately don't pull @storybook/addon-essentials: as of
// Storybook 10 the meta-addon was split, and we'd rather opt into the
// few addons we actually use than carry the whole bundle. Controls +
// actions are enabled implicitly by the default args/argTypes wiring,
// no addon line needed.
const config: StorybookConfig = {
  stories: ['../src/**/*.stories.@(ts|js)'],
  framework: {
    name: '@storybook/vue3-vite',
    options: {},
  },
  // Reuse the project's vite.config.ts (alias `@` -> src, plugins).
  // Storybook v10's @storybook/vue3-vite auto-picks vite.config.ts
  // from the project root, so there's no extra wiring here. If we
  // ever need story-only Vite overrides (e.g. mock-only env), they
  // go in viteFinal — but doing it here would diverge stories from
  // production builds.
  docs: {
    defaultName: 'Docs',
  },
}

export default config
