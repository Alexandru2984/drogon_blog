import type { Preview } from '@storybook/vue3-vite'
import { setup } from '@storybook/vue3-vite'
import { h, defineComponent } from 'vue'
// Pull the actual project styles in so stories render against the
// same look as the live app (cards, toasts, avatars, etc all rely on
// the global CSS variables defined here).
import '../src/style.css'

// Stub <router-link> globally for stories. The real component comes
// from vue-router and depends on a router instance — wiring a router
// into every story would couple the isolation goal back to app state.
// Render as an anchor with the resolved `to` swallowed (it's just for
// click navigation, irrelevant in a static visual harness).
const RouterLinkStub = defineComponent({
  name: 'RouterLink',
  props: { to: { type: [String, Object], default: '#' } },
  setup(_, { slots }) {
    return () => h('a', { href: '#' }, slots.default?.())
  },
})

setup(app => {
  app.component('RouterLink', RouterLinkStub)
})

const preview: Preview = {
  parameters: {
    controls: {
      // Auto-generate controls only for props with primitive types;
      // matchers below suppress noisy color/date controls on non-
      // color/date string props.
      matchers: {
        color: /(background|color)$/i,
        date:  /Date$/,
      },
    },
    backgrounds: {
      default: 'app',
      values: [
        { name: 'app',  value: '#f8f8f8' },
        { name: 'dark', value: '#0b0c0e' },
      ],
    },
  },
}

export default preview
