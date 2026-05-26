import { test, expect } from '@playwright/test'
import {
  registerAndLogin,
  logout,
  startLoginExpecting2fa,
  attachVirtualAuthenticator,
} from './_helpers'

// Playwright's virtual authenticator emulates a CTAP2 device entirely
// inside the Chromium process. navigator.credentials.create() and .get()
// take the codepath they would on real hardware (challenge → CBOR
// attestation/assertion), which is what we actually want to exercise
// — the WebAuthn parser, COSE key handling, sign-counter monotonicity.

test('add a passkey, then sign in with it', async ({ page }) => {
  const user = await registerAndLogin(page)

  // Wire up the virtual authenticator BEFORE we click "Add passkey",
  // otherwise navigator.credentials.create() rejects with NotAllowedError.
  await attachVirtualAuthenticator(page)

  await page.goto('/#/account/security')
  await page.getByPlaceholder(/macbook touch id/i).fill('Virtual Test Key')
  await page.getByRole('button', { name: /add passkey/i }).click()

  // The passkeys list should now contain our nickname.
  await expect(page.locator('ul li').filter({ hasText: 'Virtual Test Key' }))
    .toBeVisible({ timeout: 5_000 })

  // Sign out and back in. The login flow detects 2FA and bounces to /login/2fa.
  // The virtual authenticator from line 20 persists across the logout +
  // sign-in cycle — Chrome scopes it to the BrowserContext, not the
  // session. Re-attaching here would trip `Chrome only supports one
  // internal authenticator per environment`.
  await logout(page)
  await startLoginExpecting2fa(page, user)

  // Switch to the passkey tab and trigger the assertion. The full tab
  // label is "Passkey / security key" — anchor to that so we don't
  // double-match the "Authenticate with passkey" action button below.
  await page.getByRole('button', { name: /passkey \/ security key/i }).click()
  await page.getByRole('button', { name: /authenticate with passkey/i }).click()

  await expect(page.locator('.navbar')).toContainText(user.username, { timeout: 10_000 })
})

test('the security page lists registered passkeys after a fresh reload', async ({ page }) => {
  await registerAndLogin(page)
  await attachVirtualAuthenticator(page)

  await page.goto('/#/account/security')
  await page.getByPlaceholder(/macbook touch id/i).fill('Key A')
  await page.getByRole('button', { name: /add passkey/i }).click()
  await expect(page.locator('ul li').filter({ hasText: 'Key A' })).toBeVisible()

  // Soft navigation between routes doesn't refresh the view, so force a
  // full reload to prove the credentials really persisted to the DB and
  // re-arrive from /auth/2fa/webauthn/list on remount.
  await page.reload()
  await expect(page.locator('ul li').filter({ hasText: 'Key A' })).toBeVisible({ timeout: 5_000 })
})
