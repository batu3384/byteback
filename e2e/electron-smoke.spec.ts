import { test, expect } from '@playwright/test'
import { launchApp, closeApp } from './helpers'

test('Byteback window shows', async () => {
  const launched = await launchApp()
  const { win } = launched
  try {
    await expect(win).toHaveTitle(/Byteback/)
  } finally {
    await closeApp(launched)
  }
})

test('scan profile legend and mode buttons', async () => {
  const launched = await launchApp()
  const { win } = launched
  try {
    const legend = win.getByTestId('scan-profile-legend')
    await expect(legend).toBeVisible()
    await expect(legend).toContainText('Hızlı')
    await expect(legend).toContainText('Derin')
    await expect(legend).toContainText('Tam disk carve')
    await expect(legend).toContainText('boş')

    const quickBtn = win.getByTestId('scan-mode-quick')
    if (await quickBtn.count()) {
      await expect(quickBtn.first()).toBeVisible()
      await expect(win.getByTestId('scan-mode-deep').first()).toBeVisible()
      await expect(win.getByTestId('scan-mode-full-carve').first()).toBeVisible()
    }
  } finally {
    await closeApp(launched)
  }
})
