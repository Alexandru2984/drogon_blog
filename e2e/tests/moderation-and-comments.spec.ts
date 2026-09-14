import { test, expect, Browser, Page } from '@playwright/test'
import { registerAndLogin, setRole, TestUser } from './_helpers'

const rand = () => Math.random().toString(36).slice(2, 8)

// Each actor gets its own browser context, so its own session cookie.
async function actor(browser: Browser): Promise<{ page: Page; user: TestUser }> {
  const page = await (await browser.newContext()).newPage()
  const user = await registerAndLogin(page)
  return { page, user }
}

async function publish(page: Page, title: string, content: string): Promise<string> {
  await page.goto('/#/posts/new')
  await page.getByLabel(/title/i).fill(title)
  await page.getByLabel(/content/i).fill(content)
  await page.getByRole('button', { name: /publish/i }).click()
  await expect(page.getByRole('heading', { name: title })).toBeVisible()
  return new URL(page.url()).hash   // "#/posts/<id>"
}

async function comment(page: Page, text: string): Promise<void> {
  await page.getByPlaceholder(/write a comment/i).fill(text)
  await page.getByRole('button', { name: /post comment/i }).click()
  await expect(thread(page, text)).toBeVisible()
}

const thread = (page: Page, text: string) => page.locator('article.comment', { hasText: text })

test('authors edit and delete their own comments; a deleted parent keeps its replies', async ({ browser }) => {
  const alice = await actor(browser)
  const bob   = await actor(browser)
  const post  = await publish(alice.page, `Thread ${rand()}`, 'Say something.')

  await comment(alice.page, 'parent from alice')
  await comment(alice.page, 'leaf from alice')

  // Bob replies under Alice's parent comment, and gets no controls on hers.
  await bob.page.goto('/' + post)
  await thread(bob.page, 'parent from alice').getByRole('button', { name: 'Reply' }).click()
  const replyForm = thread(bob.page, 'parent from alice').locator('form')
  await replyForm.getByPlaceholder(/write a reply/i).fill('reply from bob')
  await replyForm.getByRole('button', { name: 'Reply' }).click()
  await expect(thread(bob.page, 'reply from bob')).toBeVisible()
  await expect(thread(bob.page, 'leaf from alice').getByRole('button', { name: 'Edit' })).toHaveCount(0)
  await expect(thread(bob.page, 'leaf from alice').getByRole('button', { name: 'Delete' })).toHaveCount(0)

  // Alice edits her leaf comment in place.
  await alice.page.goto('/' + post)
  await thread(alice.page, 'leaf from alice').getByRole('button', { name: 'Edit' }).click()
  const editing = alice.page.locator('article.comment:has(textarea)')
  await editing.locator('textarea').fill('leaf from alice, edited')
  // The post's bookmark button is also named "Save".
  await editing.getByRole('button', { name: 'Save', exact: true }).click()
  await expect(thread(alice.page, 'leaf from alice, edited')).toBeVisible()

  // A comment without replies really goes away.
  alice.page.once('dialog', d => void d.accept())
  await thread(alice.page, 'leaf from alice, edited').getByRole('button', { name: 'Delete' }).click()
  await expect(alice.page.getByText('leaf from alice, edited')).toHaveCount(0)

  // One with replies stays as a tombstone, and Bob's reply stays under it.
  let warning = ''
  alice.page.once('dialog', d => { warning = d.message(); void d.accept() })
  await thread(alice.page, 'parent from alice').getByRole('button', { name: 'Delete' }).click()
  await expect(alice.page.getByText('This comment was deleted.')).toBeVisible()
  await expect(thread(alice.page, 'reply from bob')).toBeVisible()
  expect(warning).toContain('replies')

  // The tombstone offers nobody anything to click.
  await bob.page.reload()
  await expect(bob.page.locator('article.comment', { hasText: 'This comment was deleted.' })
    .getByRole('button')).toHaveCount(0)
})

test('a reader reports a post and a moderator hides it from the queue', async ({ browser }) => {
  const author    = await actor(browser)
  const reader    = await actor(browser)
  const moderator = await actor(browser)
  await setRole(moderator.user.username, 'moderator')

  const title = `Reportable ${rand()}`
  const post  = await publish(author.page, title, 'Spam, spam, spam.')

  // An ordinary account gets the "moderators only" state, not the queue.
  await reader.page.goto('/#/moderation')
  await expect(reader.page.getByText(/for moderators and admins only/i)).toBeVisible()

  const detail = `e2e report ${rand()}`
  await reader.page.goto('/' + post)
  await reader.page.getByRole('button', { name: 'Report' }).click()
  await reader.page.getByLabel('Reason').selectOption('spam')
  await reader.page.getByLabel(/additional details/i).fill(detail)
  await reader.page.getByRole('button', { name: 'Send report' }).click()
  await expect(reader.page.getByText(/a moderator will take a look/i)).toBeVisible()

  await moderator.page.goto('/#/moderation')
  const row = moderator.page.locator('article.report-row', { hasText: detail })
  await expect(row).toBeVisible()
  await row.getByRole('button', { name: 'Take action' }).click()
  await row.getByLabel(/note/i).fill('e2e: hidden as spam')
  await row.getByRole('button', { name: 'Hide content' }).click()
  await expect(row).toHaveCount(0)

  // Hidden for everyone: gone from the feed, and the post itself is a 404.
  await reader.page.goto('/#/')
  await expect(reader.page.getByText(title)).toHaveCount(0)
  await reader.page.goto('/' + post)
  await expect(reader.page.getByText(/not found/i)).toBeVisible()
})

test('a reported comment links the moderator to the post it is on', async ({ browser }) => {
  const author    = await actor(browser)
  const reader    = await actor(browser)
  const moderator = await actor(browser)
  await setRole(moderator.user.username, 'moderator')

  const title = `Commented ${rand()}`
  const post  = await publish(author.page, title, 'Body.')
  const text  = `reportable comment ${rand()}`
  await comment(author.page, text)

  // The comment's own Report control, not the post's.
  const detail = `e2e comment report ${rand()}`
  await reader.page.goto('/' + post)
  await thread(reader.page, text).getByRole('button', { name: 'Report' }).click()
  await thread(reader.page, text).getByLabel(/additional details/i).fill(detail)
  await thread(reader.page, text).getByRole('button', { name: 'Send report' }).click()
  await expect(thread(reader.page, text).getByText(/a moderator will take a look/i)).toBeVisible()

  await moderator.page.goto('/#/moderation')
  const row = moderator.page.locator('article.report-row', { hasText: detail })
  await row.getByRole('link', { name: /view the post/i }).click()
  await expect(moderator.page.getByRole('heading', { name: title })).toBeVisible()
  await expect(thread(moderator.page, text)).toBeVisible()
})
