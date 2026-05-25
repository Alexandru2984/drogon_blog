import type { Meta, StoryObj } from '@storybook/vue3-vite'
import ToastList from './ToastList.vue'

const meta: Meta<typeof ToastList> = {
  title: 'Components/ToastList',
  component: ToastList,
}
export default meta

type Story = StoryObj<typeof ToastList>

// The three kinds the toast store can emit. App.vue mounts the list
// in the bottom-right of the viewport via fixed positioning baked
// into the .toast class.
export const AllKinds: Story = {
  args: {
    items: [
      { id: 1, text: 'Saved successfully',                  kind: 'ok'    },
      { id: 2, text: 'Could not reach the server',          kind: 'error' },
      { id: 3, text: 'Two-factor code accepted',            kind: 'info'  },
    ],
  },
}

export const Empty: Story = {
  args: { items: [] },
}
