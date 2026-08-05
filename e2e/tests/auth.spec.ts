import { test, expect } from '@playwright/test'
import { registerAndLogin, logout } from './_helpers'

test('register, login, view feed, logout', async ({ page }) => {
  const user = await registerAndLogin(page)

  // We're on the feed. Either the empty-state copy or at least one post card
  // is acceptable — the test fleet may have left posts behind, the assertion
  // is just that the feed page rendered.
  await expect(page.getByRole('heading', { name: /feed/i })).toBeVisible()

  // Logout. It lives behind the username menu now, so go through the
  // helper rather than reaching for a top-level button that is not there.
  await logout(page)
  await expect(page.locator('.navbar')).not.toContainText(user.username)
  await expect(page.getByRole('link', { name: /login/i })).toBeVisible()
})

test('login with wrong password shows an error and keeps us on the form',
  async ({ page }) => {
    await page.goto('/#/login')
    await page.getByLabel(/username/i).fill('definitely_not_a_real_user_zz')
    await page.getByLabel(/password/i).fill('wrong-password')
    await page.getByRole('button', { name: /sign in/i }).click()

    await expect(page.getByText(/invalid credentials/i)).toBeVisible()
    await expect(page).toHaveURL(/#\/login/)
  })
