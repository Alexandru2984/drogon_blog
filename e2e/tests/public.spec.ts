import { test, expect, request } from '@playwright/test'
import { registerAndLogin } from './_helpers'

test('/feed.xml returns a valid Atom feed including a fresh post',
  async ({ page, baseURL }) => {
    await registerAndLogin(page)

    const title = `Atomic ${Math.random().toString(36).slice(2, 8)}`
    await page.goto('/#/posts/new')
    await page.getByLabel(/title/i).fill(title)
    await page.getByLabel(/content/i).fill('atom body')
    await page.getByRole('button', { name: /publish/i }).click()
    await expect(page.getByRole('heading', { name: title })).toBeVisible()

    const ctx = await request.newContext({ baseURL })
    const res = await ctx.get('/feed.xml')
    expect(res.status()).toBe(200)
    expect(res.headers()['content-type']).toContain('application/atom+xml')

    const body = await res.text()
    expect(body).toContain('<feed xmlns="http://www.w3.org/2005/Atom">')
    expect(body).toContain(`<title>${title}</title>`)
  })

test('/preview/posts/{id} carries OpenGraph + Twitter card meta tags',
  async ({ page, baseURL }) => {
    await registerAndLogin(page)

    const title = `OG ${Math.random().toString(36).slice(2, 8)}`
    await page.goto('/#/posts/new')
    await page.getByLabel(/title/i).fill(title)
    await page.getByLabel(/content/i).fill('OG description body that should appear in the meta tag.')
    await page.getByRole('button', { name: /publish/i }).click()
    await expect(page.getByRole('heading', { name: title })).toBeVisible()

    // The post's id is encoded in the SPA URL after a successful create.
    const url = page.url()
    const m = url.match(/\/posts\/(\d+)/)
    expect(m).not.toBeNull()
    const id = m![1]

    const ctx = await request.newContext({ baseURL })
    const res = await ctx.get(`/preview/posts/${id}`)
    expect(res.status()).toBe(200)
    expect(res.headers()['content-type']).toContain('text/html')

    const html = await res.text()
    expect(html).toMatch(/<meta property="og:title" content="OG /)
    expect(html).toMatch(/<meta property="og:url" content=".*\/#\/posts\/\d+/)
    expect(html).toMatch(/<meta name="twitter:card" content="summary">/)
    expect(html).toMatch(/<meta http-equiv="refresh"/)
  })
