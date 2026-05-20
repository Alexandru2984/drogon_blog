import { defineConfig, devices } from '@playwright/test'

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
    baseURL:        process.env.E2E_BASE_URL ?? 'http://127.0.0.1:8092',
    trace:          'on-first-retry',
    screenshot:     'only-on-failure',
    video:          'retain-on-failure',
    actionTimeout:  10_000,
  },
  projects: [
    { name: 'chromium', use: { ...devices['Desktop Chrome'] } },
  ],
})
