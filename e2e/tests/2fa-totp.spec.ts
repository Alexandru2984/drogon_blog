import { test, expect } from '@playwright/test'
import {
  registerAndLogin,
  logout,
  startLoginExpecting2fa,
  enrollTotp,
  totpCodeNow,
} from './_helpers'

test('enroll TOTP, then log in via the TOTP challenge', async ({ page }) => {
  const user = await registerAndLogin(page)

  // Enrol — produces a shared secret + a batch of recovery codes.
  const { secret } = await enrollTotp(page)

  // Status panel should now reflect "enabled".
  await expect(page.locator('.card').filter({ hasText: 'Status' }))
    .toContainText(/authenticator app \(totp\): enabled/i)

  // Sign out and back in. The login form should bounce us to the 2FA challenge.
  await logout(page)
  await startLoginExpecting2fa(page, user)

  // Submit the current TOTP code.
  await page.locator('#totp-code').fill(totpCodeNow(secret))
  await page.getByRole('button', { name: /verify/i }).click()

  await expect(page.locator('.navbar')).toContainText(user.username, { timeout: 5_000 })
})

test('a stale TOTP code is rejected', async ({ page }) => {
  const user = await registerAndLogin(page)
  await enrollTotp(page)
  await logout(page)
  await startLoginExpecting2fa(page, user)

  // 000000 is overwhelmingly unlikely to be the current OTP at the
  // exact wall-clock second of the test. A false-positive here would
  // require the secret to be specifically crafted, which it isn't.
  await page.locator('#totp-code').fill('000000')
  await page.getByRole('button', { name: /verify/i }).click()

  await expect(page.getByText(/verification failed|invalid/i)).toBeVisible()
  await expect(page).toHaveURL(/#\/login\/2fa/)
})

test('log in with a recovery code instead of TOTP', async ({ page }) => {
  const user = await registerAndLogin(page)
  const { recoveryCodes } = await enrollTotp(page)
  expect(recoveryCodes.length).toBeGreaterThan(1)

  await logout(page)
  await startLoginExpecting2fa(page, user)

  // Switch to the recovery code tab and submit one.
  await page.getByRole('button', { name: /recovery code/i }).click()
  await page.locator('#recov-code').fill(recoveryCodes[0])
  await page.getByRole('button', { name: /sign in/i }).click()

  await expect(page.locator('.navbar')).toContainText(user.username, { timeout: 5_000 })
})

test('the same recovery code cannot be used twice', async ({ page }) => {
  const user = await registerAndLogin(page)
  const { recoveryCodes } = await enrollTotp(page)
  const oneShotCode = recoveryCodes[0]

  // First use — succeeds.
  await logout(page)
  await startLoginExpecting2fa(page, user)
  await page.getByRole('button', { name: /recovery code/i }).click()
  await page.locator('#recov-code').fill(oneShotCode)
  await page.getByRole('button', { name: /sign in/i }).click()
  await expect(page.locator('.navbar')).toContainText(user.username)

  // Second attempt — same code, fresh challenge, should fail.
  await logout(page)
  await startLoginExpecting2fa(page, user)
  await page.getByRole('button', { name: /recovery code/i }).click()
  await page.locator('#recov-code').fill(oneShotCode)
  await page.getByRole('button', { name: /sign in/i }).click()

  await expect(page.getByText(/invalid recovery code/i)).toBeVisible()
})
