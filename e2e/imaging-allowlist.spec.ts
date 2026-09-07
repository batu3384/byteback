import { test, expect } from '@playwright/test'
import { launchApp, closeApp } from './helpers'

// Main-process trust boundary: start-imaging must refuse a destination the user
// never picked through the save dialog (allowlist check fires before any native
// call), and must always answer so the renderer never waits forever.
test('start-imaging rejects a destination outside the allowlist', async () => {
  const launched = await launchApp()
  const { win } = launched
  try {
    const reply = await win.evaluate(
      () =>
        new Promise((resolve) => {
          const off = window.api.onImagingProgress((data) => {
            off()
            resolve(data)
          })
          window.api.startImaging(0, 'C:\\byteback-e2e-not-allowlisted.img')
        }),
    )
    expect(reply).toMatchObject({ current: 0, total: 0 })
    expect(String((reply as { error?: string }).error)).toContain('allowlist')

    // Refused request must not take the app down.
    await expect(win.getByRole('heading', { name: 'Byteback' })).toBeVisible()
  } finally {
    await closeApp(launched)
  }
})
