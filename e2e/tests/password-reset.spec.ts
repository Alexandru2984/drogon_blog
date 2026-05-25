import { test, expect } from '@playwright/test'
import { randomBytes } from 'node:crypto'
import {
  registerOnly,
  loginNoTwoFactor,
  uniqueUser,
  plantPasswordResetToken,
} from './_helpers'

// The end-to-end reset flow has one node we can't drive from the
// browser: the email channel. EmailHelper isn't wired in the e2e
// docker image (no live SMTP), so /auth/request-reset queues a
// message that goes nowhere. We bypass that by inserting a known
// reset token directly into password_reset_tokens — same path the
// production endpoint takes after generating the token, except we
// pick the plaintext so the rest of the flow (deep-link, form
// submit, login with new password) can run unmodified.

test('reset password via deep link, then log in with the new password',
  async ({ page }) => {
    const user = await registerOnly(page, uniqueUser())

    // Mint a random reset plaintext, plant the SHA-256 in the DB.
    const plaintext = randomBytes(32).toString('hex')
    await plantPasswordResetToken(user.email, plaintext)

    // Deep-link the SPA to the reset view with the plaintext token.
    const newPassword = `new-${plaintext.slice(0, 8)}-XYZ`
    await page.goto(`/#/reset-password?token=${plaintext}`)
    await expect(page.getByRole('heading', { name: /reset password/i })).toBeVisible()

    await page.locator('input[type="password"]').nth(0).fill(newPassword)
    await page.locator('input[type="password"]').nth(1).fill(newPassword)
    await page.getByRole('button', { name: /update password/i }).click()

    // Success bounces to /login. The new password should authenticate.
    await expect(page).toHaveURL(/#\/login/, { timeout: 5_000 })
    await loginNoTwoFactor(page, { ...user, password: newPassword })
  })

test('mismatched confirm field blocks the request before hitting the API',
  async ({ page }) => {
    const user = await registerOnly(page, uniqueUser())
    const plaintext = randomBytes(32).toString('hex')
    await plantPasswordResetToken(user.email, plaintext)

    await page.goto(`/#/reset-password?token=${plaintext}`)
    await page.locator('input[type="password"]').nth(0).fill('right-password-1234')
    await page.locator('input[type="password"]').nth(1).fill('different-password')
    await page.getByRole('button', { name: /update password/i }).click()
    await expect(page.getByText(/passwords do not match/i)).toBeVisible()
    // We're still on the reset page; navigation didn't fire.
    await expect(page).toHaveURL(/#\/reset-password/)
  })

test('the same reset token cannot be replayed after a successful use',
  async ({ page }) => {
    const user = await registerOnly(page, uniqueUser())
    const plaintext = randomBytes(32).toString('hex')
    await plantPasswordResetToken(user.email, plaintext)

    const firstPassword  = 'first-password-1234'
    const secondPassword = 'second-password-1234'

    // First use — succeeds.
    await page.goto(`/#/reset-password?token=${plaintext}`)
    await page.locator('input[type="password"]').nth(0).fill(firstPassword)
    await page.locator('input[type="password"]').nth(1).fill(firstPassword)
    await page.getByRole('button', { name: /update password/i }).click()
    await expect(page).toHaveURL(/#\/login/, { timeout: 5_000 })

    // Second attempt with the same plaintext should fail at the API.
    await page.goto(`/#/reset-password?token=${plaintext}`)
    await page.locator('input[type="password"]').nth(0).fill(secondPassword)
    await page.locator('input[type="password"]').nth(1).fill(secondPassword)
    await page.getByRole('button', { name: /update password/i }).click()
    await expect(page.getByText(/invalid|expired|reset failed/i)).toBeVisible()
  })
