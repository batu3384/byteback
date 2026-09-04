import { test, expect } from '@playwright/test'
import { launchApp, closeApp } from './helpers'

test('report generate button disabled without scan', async () => {
  const launched = await launchApp()
  const { win } = launched
  try {
    await expect(win.getByTestId('nav-report')).toBeDisabled()
  } finally {
    await closeApp(launched)
  }
})
