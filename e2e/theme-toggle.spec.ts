import { test, expect } from '@playwright/test'
import { launchApp, closeApp } from './helpers'

test.describe('Theme toggle', () => {
  // The paused-session resume banner cannot be seeded: seedScanFixture always
  // marks the scan complete (native bridge_scan.cpp calls completeScan), so
  // status=4 is unreachable via the fixture API. The theme toggle is the
  // fallback assertion for this sweep.
  test('theme button flips data-theme and flips back', async () => {
    const launched = await launchApp()
    const { win } = launched
    try {
      await expect(win.locator('html')).toHaveAttribute('data-theme', 'dark')
      await win.getByRole('button', { name: 'Açık temaya geç' }).click()
      await expect(win.locator('html')).toHaveAttribute('data-theme', 'light')
      // The button's own label flips with the theme.
      await win.getByRole('button', { name: 'Koyu temaya geç' }).click()
      await expect(win.locator('html')).toHaveAttribute('data-theme', 'dark')
    } finally {
      await closeApp(launched)
    }
  })
})
