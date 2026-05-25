import type { Meta, StoryObj } from '@storybook/vue3-vite'
import PostCard from './PostCard.vue'

// Storybook 8/9/10 expects the meta object to be the default export.
// Title controls the sidebar grouping; '/' nests under "Components".
const meta: Meta<typeof PostCard> = {
  title: 'Components/PostCard',
  component: PostCard,
  argTypes: {
    clamp: { control: 'boolean' },
  },
}
export default meta

type Story = StoryObj<typeof PostCard>

const shortPost = {
  id: 1,
  title: 'PostgreSQL tsvector primer',
  content: 'GIN indexes store the lexemes pulled out of a tsvector; ts_rank weights them by document.',
  content_html: '<p>GIN indexes store the lexemes pulled out of a tsvector…</p>',
  created_at: '2026-05-01 09:30:00',
  updated_at: '2026-05-01 09:30:00',
  author: {
    id: 9,
    username: 'micu',
    profile_image: '',
  },
}

const longPost = {
  ...shortPost,
  id: 2,
  title: 'Why N+1 queries hurt and how a JOIN crushes them',
  content: 'Repeat after me: every framework that ' +
           'lets you write `for row in rows: row.related.load()` is ' +
           'one bad commit away from a 200 ms request because the ' +
           'inner loop did 200 round-trips. The fix is usually a ' +
           'three-line LEFT JOIN, not a caching layer. '.repeat(3),
}

export const Default: Story = {
  args: { post: shortPost, clamp: false },
}

export const Clamped: Story = {
  args: { post: longPost, clamp: true },
}

export const NoAuthor: Story = {
  // What the feed renders when the author row was deleted concurrently
  // — the LEFT JOIN keeps the post but drops the author payload.
  args: { post: { ...shortPost, author: undefined }, clamp: false },
}
