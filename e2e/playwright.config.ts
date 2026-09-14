import { defineConfig, devices } from '@playwright/test'
import { existsSync } from 'node:fs'

// There is deliberately no default target. The old one, 127.0.0.1:8092, is
// the live service on the host this repository deploys from, and every run
// there registered accounts and wrote posts into the production database —
// some of those accounts are still in it. Point E2E_BASE_URL at a disposable
// stack (see README.md in this directory).
const baseURL = process.env.E2E_BASE_URL
if (!baseURL) {
  throw new Error(
    'E2E_BASE_URL is not set. Point it at a disposable stack (see e2e/README.md), never at production.')
}

// A second guard for the production host itself, where port 8092 is the
// live server, and for the public site.
const target = new URL(baseURL)
const onProductionHost = existsSync('/etc/systemd/system/drogon-blog.service')
const isProduction =
  (onProductionHost && target.port === '8092') || /(^|\.)micutu\.com$/.test(target.hostname)
if (isProduction && process.env.E2E_ALLOW_PRODUCTION !== '1') {
  throw new Error(
    `Refusing to run the e2e suite against ${baseURL}: that is production, and every spec writes to it.`)
}

// The blog uses hash routing, so navigation between routes never reloads
// the page; sequential tests share faster setup. CI uses workers=1 to keep
// per-test register/login deterministic against the rate limiter.
export default defineConfig({
  testDir:    './tests',
  timeout:    30_000,
  retries:    process.env.CI ? 1 : 0,
  workers:    1,
  reporter:   process.env.CI ? [['github'], ['html', { open: 'never' }]] : 'list',
  use: {
    baseURL,
    trace:          'on-first-retry',
    screenshot:     'only-on-failure',
    video:          'retain-on-failure',
    actionTimeout:  10_000,
  },
  projects: [
    { name: 'chromium', use: { ...devices['Desktop Chrome'] } },
  ],
})
