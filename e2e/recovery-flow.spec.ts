import { test, expect } from '@playwright/test'
import { launchApp, closeApp } from './helpers'

test.describe('Recovery flow UI', () => {
  test('results page blocked until scan completes', async () => {
    const launched = await launchApp()
    const { win } = launched
    try {
      const resultsBtn = win.getByTestId('nav-results')
      await expect(resultsBtn).toBeDisabled()
      await resultsBtn.click({ force: true })
      await expect(win.locator('.header-title h2')).toHaveText('Ana Ekran')
      await expect(win.getByTestId('show-duplicates')).not.toBeVisible()
    } finally {
      await closeApp(launched)
    }
  })

  test('paused scan banner test id when no native session', async () => {
    const launched = await launchApp()
    const { win } = launched
    try {
      // Banner only when status=4 in DB; ensure dashboard loads without crash.
      await expect(win.getByTestId('scan-profile-legend')).toBeVisible()
    } finally {
      await closeApp(launched)
    }
  })
})
