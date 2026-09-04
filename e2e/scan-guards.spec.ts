import { test, expect } from '@playwright/test'
import { launchApp, closeApp } from './helpers'

test('scan-dependent sidebar items disabled without completed scan', async () => {
  const launched = await launchApp()
  const { win } = launched
  try {
    await expect(win.getByTestId('nav-report')).toBeDisabled()
    await expect(win.getByTestId('nav-results')).toBeDisabled()
    await expect(win.getByTestId('nav-search')).toBeDisabled()
    await expect(win.getByTestId('nav-timeline')).toBeDisabled()
  } finally {
    await closeApp(launched)
  }
})

test('scan-dependent nav stays on dashboard when report is forced', async () => {
  const launched = await launchApp()
  const { win } = launched
  try {
    const reportBtn = win.getByTestId('nav-report')
    await expect(reportBtn).toBeDisabled()
    await reportBtn.click({ force: true })
    await expect(win.locator('.header-title h2')).toHaveText('Ana Ekran')
  } finally {
    await closeApp(launched)
  }
})
