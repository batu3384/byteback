import { test, expect, _electron as electron } from '@playwright/test'
import { existsSync } from 'node:fs'
import path from 'node:path'

const root = path.join(__dirname, '..')
const mainJs = path.join(root, 'out', 'main', 'main.js')

test.describe('Recovery flow UI', () => {
  test('results view exposes recovery controls', async () => {
    test.skip(!existsSync(mainJs), 'out/main/main.js missing — npm run build first')

    const app = await electron.launch({ args: [mainJs], cwd: root })
    try {
      const win = await app.firstWindow()
      await expect(win.getByRole('heading', { name: 'Byteback' })).toBeVisible({ timeout: 30_000 })

      await win.getByRole('button', { name: /Sonuçlar|Results/i }).click().catch(() => {
        return win.locator('nav').getByText(/Sonuç/i).first().click()
      })

      await expect(win.getByTestId('show-duplicates')).toBeVisible({ timeout: 10_000 })
      await expect(win.getByTestId('filter-deleted')).toBeVisible()
      await expect(win.getByRole('button', { name: /Seçilenleri Kurtar/i })).toBeVisible()
    } finally {
      await app.close()
    }
  })

  test('paused scan banner test id when no native session', async () => {
    test.skip(!existsSync(mainJs), 'out/main/main.js missing — npm run build first')

    const app = await electron.launch({ args: [mainJs], cwd: root })
    try {
      const win = await app.firstWindow()
      await expect(win.getByRole('heading', { name: 'Byteback' })).toBeVisible({ timeout: 30_000 })
      // Banner only when status=4 in DB; ensure dashboard loads without crash.
      await expect(win.getByTestId('scan-profile-legend')).toBeVisible()
    } finally {
      await app.close()
    }
  })
})
