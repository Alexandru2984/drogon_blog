import { Page, BrowserContext, expect, CDPSession } from '@playwright/test'
import { generateSync } from 'otplib'
import { Client as PgClient } from 'pg'
import { createHash } from 'node:crypto'

// ---------------------------------------------------------------- Users

export type TestUser = {
  username: string
  email:    string
  password: string
}

// Each test mints a fresh user so runs are independent. The suffix is plenty
// random because the suite runs serially and IDs are monotonic.
export function uniqueUser(): TestUser {
  const suffix = Math.random().toString(36).slice(2, 10)
  return {
    username: `e2e_${suffix}`,
    email:    `e2e_${suffix}@example.test`,
    password: 'e2e-password-1234',
  }
}

// Bare register call without auto-login. Used by tests that need to drive the
// login flow themselves (2FA challenges, deliberate wrong-password assertions).
export async function registerOnly(page: Page, user: TestUser = uniqueUser()): Promise<TestUser> {
  await page.goto('/#/register')
  await page.getByLabel(/username/i).fill(user.username)
  await page.getByLabel(/email/i).fill(user.email)
  await page.getByLabel(/password/i).fill(user.password)
  await page.getByRole('button', { name: /sign up/i }).click()
  await expect(page).toHaveURL(/#\/login/, { timeout: 5_000 })
  return user
}

// Drives /login through to a logged-in feed. Returns once the navbar shows
// the username (proxy for "session cookie installed + /auth/me succeeded").
// Use this only when the account has no 2FA enrolled; loginWith2faChallenge
// covers the 2FA branch.
export async function loginNoTwoFactor(page: Page, user: TestUser): Promise<void> {
  await page.goto('/#/login')
  await page.getByLabel(/username/i).fill(user.username)
  await page.getByLabel(/password/i).fill(user.password)
  await page.getByRole('button', { name: /sign in/i }).click()
  await expect(page.locator('.navbar')).toContainText(user.username, { timeout: 5_000 })
}

// register + login via the SPA. Returns once /auth/me confirms the session.
// Kept as the smoke-test default for non-2FA flows. Avoids a redundant
// goto('/#/login') after registerOnly — the SPA already hash-routes
// there on success.
export async function registerAndLogin(page: Page, user: TestUser = uniqueUser()): Promise<TestUser> {
  await registerOnly(page, user)
  await page.getByLabel(/username/i).fill(user.username)
  await page.getByLabel(/password/i).fill(user.password)
  await page.getByRole('button', { name: /sign in/i }).click()
  await expect(page.locator('.navbar')).toContainText(user.username, { timeout: 5_000 })
  return user
}

// Drives the password form on /login, expects the SPA to navigate to
// /login/2fa (the challenge UI). Does NOT submit the 2nd factor — the
// caller picks the method.
export async function startLoginExpecting2fa(page: Page, user: TestUser): Promise<void> {
  await page.goto('/#/login')
  await page.getByLabel(/username/i).fill(user.username)
  await page.getByLabel(/password/i).fill(user.password)
  await page.getByRole('button', { name: /sign in/i }).click()
  await expect(page).toHaveURL(/#\/login\/2fa/, { timeout: 5_000 })
}

// Drives the SPA's logout button. Some tests rely on a clean cookie jar
// between two phases of the same browser context.
//
// Logout is no longer a top-level navbar button: the account-scoped items
// (profile, drafts, saved, 2FA, your data, logout) moved behind the
// username menu when twelve items stopped fitting on one row at 1440 px.
// So the menu has to be opened first — and on a narrow viewport the whole
// bar collapses into the drawer, which is a different control again.
// The two shells do not expose the same control, so this cannot be one
// role query:
//
//   desktop  <button role="menuitem" class="account-logout">Logout</button>
//   drawer   <button>Logout</button>
//
// The explicit role="menuitem" replaces the implicit button role, so
// getByRole('button') does not match the desktop one at all — which is
// how this helper spent six specs timing out on a menu that was open in
// front of it.
export async function logout(page: Page): Promise<void> {
  const shell = await openAccountMenu(page)

  if (shell === 'desktop') {
    await page.locator('#account-menu .account-logout').click()
  } else {
    await page.locator('#mobile-drawer')
      .getByRole('button', { name: /logout/i })
      .click()
  }

  await expect(page.getByRole('link', { name: /login/i })).toBeVisible({ timeout: 5_000 })
}

// Reveals the account-scoped nav items (profile, drafts, saved, 2FA, your
// data, logout), which moved off the navbar when twelve items stopped
// fitting on one row at 1440 px. Returns which shell answered so the
// caller can address the right control.
//
// Idempotent: an already-open menu is left alone. Throws rather than
// returning quietly when neither shell is on screen — a silent no-op here
// surfaces later as an opaque 10 s timeout on whatever the caller clicks.
export async function openAccountMenu(page: Page): Promise<'desktop' | 'drawer'> {
  const trigger = page.locator('.account-trigger')
  if (await trigger.isVisible().catch(() => false)) {
    if ((await trigger.getAttribute('aria-expanded')) !== 'true') await trigger.click()
    await expect(page.locator('#account-menu')).toBeVisible({ timeout: 5_000 })
    return 'desktop'
  }

  // Narrow shell: the hamburger drawer carries the same links.
  const burger = page.locator('.nav-toggle')
  if (await burger.isVisible().catch(() => false)) {
    if ((await burger.getAttribute('aria-expanded')) !== 'true') await burger.click()
    await expect(page.locator('#mobile-drawer')).toBeVisible({ timeout: 5_000 })
    return 'drawer'
  }

  throw new Error(
    'openAccountMenu: neither .account-trigger nor .nav-toggle is visible — ' +
    'is the page signed in and has the navbar rendered?')
}

// ---------------------------------------------------------------- 2FA / TOTP

// Walks /account/security to provision a TOTP factor for the currently
// signed-in user. Returns the shared secret (so the test can compute
// matching codes for subsequent challenges) plus the one-time recovery
// codes shown immediately after enrolment.
export async function enrollTotp(page: Page, password: string): Promise<{
  secret: string
  recoveryCodes: string[]
}> {
  await page.goto('/#/account/security')
  await page.getByLabel(/current password for 2fa changes/i).fill(password)
  await page.getByRole('button', { name: /begin setup/i }).click()

  // The secret sits in .totp-secret. It used to be an inline <code>, but a
  // 32-character base32 string in an inline element ran past the edge of
  // the card on a phone, so it became a block during the responsive pass.
  // Selecting on the class rather than the tag is also what keeps this test
  // from breaking the next time the element changes.
  const secret = (await page.locator('.totp-secret').first().innerText()).trim()
  expect(secret).toMatch(/^[A-Z2-7]+$/) // base32 alphabet

  // Compute the current code from the captured secret.
  const code = generateSync({ secret })

  await page.locator('#totp-confirm').fill(code)
  await page.getByRole('button', { name: /confirm/i }).click()

  // After confirm, the page swaps to the "Save your recovery codes" card.
  await expect(page.getByRole('heading', { name: /save your recovery codes/i }))
    .toBeVisible({ timeout: 5_000 })
  const codesBlob = await page.locator('pre').first().innerText()
  const recoveryCodes = codesBlob.split('\n').map(s => s.trim()).filter(Boolean)
  expect(recoveryCodes.length).toBeGreaterThan(0)

  // Acknowledge + dismiss so the panel doesn't shadow later assertions.
  await page.getByLabel(/i have saved them/i).check()
  await page.getByRole('button', { name: /dismiss/i }).click()

  return { secret, recoveryCodes }
}

// Compute a TOTP code now, given the shared secret.
export function totpCodeNow(secret: string): string {
  return generateSync({ secret })
}

// ---------------------------------------------------------------- WebAuthn

// Attach a virtual authenticator to the page via CDP. Returns the cdp
// session so the caller can later remove the authenticator if needed —
// in practice the page context closes at end of test and tears it down
// automatically.
//
// Defaults to ctap2 + internal transport + resident keys, which is
// enough to satisfy the blog's PublicKeyCredentialCreationOptions
// (authenticator-attached = "platform", user-verification = "preferred").
export async function attachVirtualAuthenticator(page: Page): Promise<CDPSession> {
  const cdp = await page.context().newCDPSession(page)
  await cdp.send('WebAuthn.enable')
  await cdp.send('WebAuthn.addVirtualAuthenticator', {
    options: {
      protocol:                 'ctap2',
      transport:                'internal',
      hasResidentKey:           true,
      hasUserVerification:      true,
      isUserVerified:           true,
      automaticPresenceSimulation: true,
    },
  })
  return cdp
}

// ---------------------------------------------------------------- DB shortcut

// Direct DB client for tests that have to bypass the email channel
// (password reset). The test runner picks up the same env vars CI uses;
// localhost defaults match the docker-compose.e2e.yml port mapping below.
function pgClientFromEnv(): PgClient {
  return new PgClient({
    host:     process.env.E2E_DB_HOST     ?? '127.0.0.1',
    port:     Number(process.env.E2E_DB_PORT ?? 55432),
    user:     process.env.E2E_DB_USER     ?? 'blog_user',
    password: process.env.E2E_DB_PASSWORD ?? 'e2e_db_password',
    database: process.env.E2E_DB_NAME     ?? 'blog_db',
  })
}

// Plants a reset token for the given email directly in
// password_reset_tokens. The DB stores SHA-256 of the plaintext, so
// the caller passes the plaintext (which would normally arrive via
// email) and we hash it for storage. Returns the user id touched.
export async function plantPasswordResetToken(
  email:           string,
  plaintextToken:  string,
  expiresInMinutes = 30,
): Promise<number> {
  const c = pgClientFromEnv()
  await c.connect()
  try {
    const u = await c.query<{ id: number }>(
      'SELECT id FROM users WHERE email = $1', [email])
    if (u.rows.length === 0) throw new Error(`no user with email ${email}`)
    const userId = u.rows[0].id

    const tokenHash = createHash('sha256').update(plaintextToken).digest('hex')
    await c.query('DELETE FROM password_reset_tokens WHERE user_id = $1', [userId])
    await c.query(
      `INSERT INTO password_reset_tokens (user_id, token, expires_at)
       VALUES ($1, $2, NOW() + ($3 || ' minutes')::interval)`,
      [userId, tokenHash, expiresInMinutes],
    )
    return userId
  } finally {
    await c.end()
  }
}

// Convenience: returns the integer id of a user by username, for tests
// that need to address peers via /messages/conversation/{userId}.
export async function userIdByName(username: string): Promise<number> {
  const c = pgClientFromEnv()
  await c.connect()
  try {
    const r = await c.query<{ id: number }>(
      'SELECT id FROM users WHERE username = $1', [username])
    if (r.rows.length === 0) throw new Error(`no user with username ${username}`)
    return r.rows[0].id
  } finally {
    await c.end()
  }
}
