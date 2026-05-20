import { Page, expect } from '@playwright/test'

// Each test mints a fresh user so runs are independent. The suffix is plenty
// random because the suite runs serially and IDs are monotonic.
export function uniqueUser() {
  const suffix = Math.random().toString(36).slice(2, 10)
  return {
    username: `e2e_${suffix}`,
    email:    `e2e_${suffix}@example.test`,
    password: 'e2e-password-1234',
  }
}

// register + login via the SPA. Returns once /auth/me confirms the session.
export async function registerAndLogin(page: Page) {
  const user = uniqueUser()

  await page.goto('/#/register')
  await page.getByLabel(/username/i).fill(user.username)
  await page.getByLabel(/email/i).fill(user.email)
  await page.getByLabel(/password/i).fill(user.password)
  await page.getByRole('button', { name: /sign up/i }).click()

  // Registration UI bounces to /login after success.
  await expect(page).toHaveURL(/#\/login/, { timeout: 5_000 })

  await page.getByLabel(/username/i).fill(user.username)
  await page.getByLabel(/password/i).fill(user.password)
  await page.getByRole('button', { name: /sign in/i }).click()

  // After login the SPA goes back to the feed; the navbar shows the username.
  await expect(page.locator('.navbar')).toContainText(user.username,
    { timeout: 5_000 })

  return user
}
