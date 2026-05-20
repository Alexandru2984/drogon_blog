import { test, expect } from '@playwright/test'
import { registerAndLogin } from './_helpers'

test('create a post, see it in feed, open it, comment on it', async ({ page }) => {
  await registerAndLogin(page)

  const title   = `Hello ${Math.random().toString(36).slice(2, 8)}`
  const content = '# Heading\n\n**bold** body with `code` and a link to <https://example.org>.'

  await page.goto('/#/posts/new')
  await page.getByLabel(/title/i).fill(title)
  await page.getByLabel(/content/i).fill(content)
  await page.getByRole('button', { name: /publish/i }).click()

  // Lands on the post detail page; markdown should be rendered server-side.
  await expect(page.getByRole('heading', { name: title })).toBeVisible()
  await expect(page.locator('.post-body strong')).toContainText('bold')
  await expect(page.locator('.post-body code')).toContainText('code')

  // Comment on the post — input + submit, then see the comment appear.
  await page.getByPlaceholder(/write a comment/i).fill('first reply')
  await page.getByRole('button', { name: /post comment/i }).click()
  await expect(page.locator('article.card').filter({ hasText: 'first reply' })).toBeVisible()

  // Open the feed and confirm the post is at the top.
  await page.goto('/#/')
  await expect(page.locator('article.card').first()).toContainText(title)
})

test('search finds the freshly-created post', async ({ page }) => {
  await registerAndLogin(page)

  const marker = `tsmarker${Math.random().toString(36).slice(2, 8)}`

  await page.goto('/#/posts/new')
  await page.getByLabel(/title/i).fill(`Searchable ${marker}`)
  await page.getByLabel(/content/i).fill(`This post contains the ${marker} marker.`)
  await page.getByRole('button', { name: /publish/i }).click()
  await expect(page.getByRole('heading', { name: /Searchable/i })).toBeVisible()

  // Run search via the navbar input.
  await page.locator('.nav-search input').fill(marker)
  await page.locator('.nav-search input').press('Enter')

  await expect(page.getByRole('heading', { name: /Search/i })).toBeVisible()
  await expect(page.getByText(`Searchable ${marker}`)).toBeVisible()
  // Snippet should highlight the marker via <mark> from ts_headline.
  await expect(page.locator('.snippet mark').first()).toBeVisible()
})
