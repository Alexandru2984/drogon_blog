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
  await page.getByLabel(/current password for 2fa changes/i).fill(user.password)
  await page.getByPlaceholder(/macbook touch id/i).fill('Virtual Test Key')

  // Deliberately start and abandon one registration. The next UI action must
  // replace this challenge; Drogon Session::insert() does not overwrite, so
  // the old implementation returned an unverifiable second challenge here.
  const abandonedRegistration = await page.evaluate(async (password) => {
    const cookie = document.cookie.split('; ').find(part =>
      part.startsWith('csrf_token=') || part.startsWith('__Host-csrf_token='))
    const csrf = cookie ? decodeURIComponent(cookie.slice(cookie.indexOf('=') + 1)) : ''
    const response = await fetch('/auth/2fa/webauthn/register/begin', {
      method: 'POST',
      headers: {
        'Content-Type': 'application/json',
        'X-CSRF-Token': csrf,
      },
      body: JSON.stringify({ password }),
    })
    return {
      status: response.status,
      cacheControl: response.headers.get('cache-control') ?? '',
    }
  }, user.password)
  expect(abandonedRegistration.status).toBe(200)
  expect(abandonedRegistration.cacheControl).toContain('no-store')

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

  // Switch to the passkey tab and trigger the assertion. The tab is a
  // <button role="tab">, so its accessible role is tab, not button —
  // which is also what keeps it from colliding with the "Authenticate
  // with passkey" action button inside the panel.
  await page.getByRole('tab', { name: /passkey/i }).click()

  // Same regression on login: discard one challenge, then let the UI request
  // the replacement it will actually sign. The replacement must be the value
  // retained server-side and every challenge response must be non-cacheable.
  const abandonedLogin = await page.evaluate(async () => {
    const response = await fetch('/auth/login/verify-webauthn/begin', {
      method: 'POST',
    })
    return {
      status: response.status,
      cacheControl: response.headers.get('cache-control') ?? '',
    }
  })
  expect(abandonedLogin.status).toBe(200)
  expect(abandonedLogin.cacheControl).toContain('no-store')

  await page.getByRole('button', { name: /authenticate with passkey/i }).click()

  await expect(page.locator('.navbar')).toContainText(user.username, { timeout: 10_000 })
})

test('the security page lists registered passkeys after a fresh reload', async ({ page }) => {
  const user = await registerAndLogin(page)
  await attachVirtualAuthenticator(page)

  await page.goto('/#/account/security')
  await page.getByLabel(/current password for 2fa changes/i).fill(user.password)
  await page.getByPlaceholder(/macbook touch id/i).fill('Key A')
  await page.getByRole('button', { name: /add passkey/i }).click()
  await expect(page.locator('ul li').filter({ hasText: 'Key A' })).toBeVisible()

  // Soft navigation between routes doesn't refresh the view, so force a
  // full reload to prove the credentials really persisted to the DB and
  // re-arrive from /auth/2fa/webauthn/list on remount.
  await page.reload()
  await expect(page.locator('ul li').filter({ hasText: 'Key A' })).toBeVisible({ timeout: 5_000 })
})
