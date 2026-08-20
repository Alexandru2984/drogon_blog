import { test, expect } from '@playwright/test'

test('mobile navigation is named, focus-trapped, localized, and restores focus',
  async ({ page }) => {
    await page.setViewportSize({ width: 375, height: 812 })
    await page.goto('/#/')

    await expect(page).toHaveTitle("Feed · Micu's Blog")

    const toggle = page.locator('.nav-toggle')
    await expect(toggle).toHaveAttribute('aria-label', 'Open menu')
    await toggle.click()

    const drawer = page.getByRole('dialog', { name: 'Menu' })
    await expect(drawer).toBeVisible()
    const close = drawer.getByRole('button', { name: 'Close menu' })
    await expect(close).toBeFocused()

    // Both ends wrap: reverse from the first control reaches the last, then
    // forward from the last returns to the first.
    await page.keyboard.press('Shift+Tab')
    await expect(drawer.getByRole('combobox', { name: 'Language' })).toBeFocused()
    await page.keyboard.press('Tab')
    await expect(close).toBeFocused()

    await close.click()
    await expect(toggle).toBeFocused()

    await toggle.click()
    await drawer.getByRole('link', { name: 'Login' }).click()
    await expect(page.getByRole('heading', { name: 'Sign in', exact: true })).toBeVisible()
    await expect(page).toHaveTitle("Sign in · Micu's Blog")
    await expect(page.getByRole('status').first()).toContainText('Sign in loaded')

    await toggle.click()
    await drawer.getByRole('combobox', { name: 'Language' }).selectOption('ro')
    await expect(page.locator('html')).toHaveAttribute('lang', 'ro')
    await expect(page).toHaveTitle("Autentificare · Micu's Blog")
    await expect(toggle).toHaveAttribute('aria-label', 'Deschide meniul')
  })

test('the responsive header has 44px targets and no horizontal overflow',
  async ({ page }) => {
    await page.goto('/#/')

    for (const width of [320, 768, 1024, 1120, 1121, 1440]) {
      await page.setViewportSize({ width, height: 800 })

      const metrics = await page.evaluate(() => ({
        scrollWidth: document.documentElement.scrollWidth,
        clientWidth: document.documentElement.clientWidth,
        targets: Array.from(document.querySelectorAll<HTMLElement>(
          '.navbar a, .navbar button, .navbar select, .navbar input',
        )).filter((element) => {
          const rect = element.getBoundingClientRect()
          return rect.width > 0 && rect.height > 0
        }).map((element) => Math.round(element.getBoundingClientRect().height)),
      }))

      expect(metrics.scrollWidth).toBe(metrics.clientWidth)
      expect(metrics.targets.length).toBeGreaterThan(0)
      expect(metrics.targets.every(height => height >= 44)).toBe(true)

      if (width <= 1120) {
        await expect(page.locator('.nav-toggle')).toBeVisible()
        await expect(page.locator('.nav-links')).toBeHidden()
      } else {
        await expect(page.locator('.nav-toggle')).toBeHidden()
        await expect(page.locator('.nav-links')).toBeVisible()
      }
    }
  })

test('the tags route reaches the backend and leaves its loading state',
  async ({ page }) => {
    await page.goto('/#/tags')

    await expect(page.getByRole('heading', { name: 'Tags', exact: true })).toBeVisible()
    await expect(page.getByText('Loading tags…')).toHaveCount(0)
    await expect(page.locator('.cloud, .empty-state')).toBeVisible()
    await expect(page.getByRole('alert')).toHaveCount(0)
  })
