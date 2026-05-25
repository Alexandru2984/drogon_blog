import type { Meta, StoryObj } from '@storybook/vue3-vite'
import Avatar from './Avatar.vue'

const meta: Meta<typeof Avatar> = {
  title: 'Components/Avatar',
  component: Avatar,
}
export default meta

type Story = StoryObj<typeof Avatar>

// The .avatar class lays down a fixed-size circle; with no image url
// the CSS variable fallback paints a neutral grey. This is what we
// render anywhere the user has never uploaded a profile picture.
export const Placeholder: Story = {
  args: { imageUrl: '' },
}

export const Photo: Story = {
  // A real upload URL would be /uploads/<user>/<random>.jpg; for a
  // story we point at a stable Wikimedia portrait so the visual
  // doesn't depend on what's in the prod uploads dir.
  args: { imageUrl: 'https://upload.wikimedia.org/wikipedia/commons/thumb/d/d5/Avatar_in_the_Wizardry_VIII_video_game.jpg/120px-Avatar_in_the_Wizardry_VIII_video_game.jpg' },
}
