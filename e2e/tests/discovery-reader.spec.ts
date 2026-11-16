import { test, expect } from '@playwright/test'
import { Client } from 'pg'

const slug = `discover-${Date.now()}`
let authorId: number
let likedId: number
let discussedId: number
let tagId: number
function db() {
  return new Client({ host: process.env.E2E_DB_HOST ?? '127.0.0.1',
    port: Number(process.env.E2E_DB_PORT ?? 55432), user: process.env.E2E_DB_USER ?? 'blog_user',
    password: process.env.E2E_DB_PASSWORD ?? 'e2e_db_password', database: process.env.E2E_DB_NAME ?? 'blog_db' })
}

test.beforeAll(async () => {
  const client = db()
  await client.connect()
  try {
    authorId = (await client.query(`INSERT INTO users(username,email,password_hash)
      VALUES($1,$2,'not-a-login-hash') RETURNING id`, [slug, `${slug}@example.test`])).rows[0].id
    tagId = (await client.query('INSERT INTO tags(slug,label) VALUES($1,$1) RETURNING id', [slug])).rows[0].id
    const html = Array.from({ length: 30 }, (_, i) => `<h2>Section ${i + 1}</h2><p>${'A thoughtful article with readable text. '.repeat(12)}</p>`).join('')
    for (const [title, state, minutes] of [
      ['Liked article', 'published', 3], ['Discussed article', 'published', 8],
      ['Private draft secret', 'draft', 3], ['Hidden post secret', 'hidden', 3],
      ['Older article', 'old', 20],
    ] as const) {
      const result = await client.query(`INSERT INTO posts(user_id,title,content,content_html,excerpt,reading_minutes,published_at,hidden_at)
        VALUES($1,$2,'Article source',$3,$2,$4,
          CASE WHEN $5='draft' THEN NULL WHEN $5='old' THEN now()-interval '60 days' ELSE now() END,
          CASE WHEN $5='hidden' THEN now() ELSE NULL END) RETURNING id`, [authorId, title, html, minutes, state])
      const id = result.rows[0].id
      await client.query('INSERT INTO post_tags(post_id,tag_id) VALUES($1,$2)', [id, tagId])
      if (title === 'Liked article') likedId = id
      if (title === 'Discussed article') discussedId = id
    }
    await client.query('INSERT INTO likes(post_id,user_id) VALUES($1,$2)', [likedId, authorId])
    await client.query("INSERT INTO comments(post_id,user_id,content) VALUES($1,$2,'Visible comment')", [discussedId, authorId])
    await client.query("INSERT INTO comments(post_id,user_id,content,hidden_at) VALUES($1,$2,'Hidden comment',now())", [likedId, authorId])
    await client.query("INSERT INTO comments(post_id,user_id,content,deleted_at) VALUES($1,$2,'Deleted comment',now())", [likedId, authorId])
  } finally { await client.end() }
})

test.afterAll(async () => {
  const client = db()
  await client.connect()
  try {
    if (authorId) await client.query('DELETE FROM users WHERE id=$1', [authorId])
    if (tagId) await client.query('DELETE FROM tags WHERE id=$1', [tagId])
  } finally { await client.end() }
})

test('discovery sorts real engagement, filters and paginates only public posts', async ({ request }) => {
  const get = async (query: string) => {
    const response = await request.get(`/posts/discover?tag=${slug}&${query}`)
    expect(response.ok()).toBeTruthy()
    return response.json()
  }
  const popular = await get('sort=popular&limit=1')
  expect(popular.posts.map((p: { id: number }) => p.id)).toEqual([likedId])
  expect(popular.posts[0].comment_count).toBe(0)
  expect(popular.next_offset).toBe(1)
  const second = await get('sort=popular&limit=1&offset=1')
  expect(second.posts[0].id).toBe(discussedId)
  const discussed = await get('sort=discussed&period=week')
  expect(discussed.posts.map((p: { id: number }) => p.id)).toEqual([discussedId, likedId])
  const short = await get('max_minutes=5')
  expect(short.posts.map((p: { id: number }) => p.id)).toEqual([likedId])
  expect(short.next_offset).toBeNull()
  const all = await get('sort=latest')
  expect(all.posts).toHaveLength(3)
  expect(JSON.stringify(all)).not.toContain('secret')
  expect((await request.get('/posts/discover?sort=title;DROP')).status()).toBe(400)
  expect((await request.get('/posts/discover?offset=2001')).status()).toBe(400)
})

test('discovery controls keep shareable filters, recover from failure, and fit mobile', async ({ page }) => {
  await page.setViewportSize({ width: 320, height: 740 })
  await page.goto(`/#/?tag=${slug}&feed=popular`)
  await expect(page.locator('.post-card-title').first()).toContainText('Liked article')
  await page.getByText('Explore posts', { exact: false }).first().click()
  await page.getByLabel('Reading time', { exact: true }).selectOption('5')
  await expect(page).toHaveURL(/read=5/)
  await expect(page.locator('.post-card')).toHaveCount(1)
  await page.reload()
  await page.getByText('Explore posts', { exact: false }).first().click()
  await expect(page.getByLabel('Reading time', { exact: true })).toHaveValue('5')
  expect(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth)).toBeTruthy()
  await page.route('**/posts/discover*', route => route.fulfill({ status: 503, json: { error: 'Temporary failure' } }))
  await page.getByRole('button', { name: 'Most discussed', exact: true }).click()
  await expect(page.getByRole('alert')).toContainText('Temporary failure')
  await page.unroute('**/posts/discover*')
  await page.getByRole('button', { name: 'Try again', exact: true }).click()
  await expect(page.locator('.post-card')).toHaveCount(1)
})
