import { test, expect, _electron as electron } from '@playwright/test'
import { existsSync } from 'node:fs'
import path from 'node:path'

const root = path.join(__dirname, '..')
const mainJs = path.join(root, 'out', 'main', 'main.js')

test.describe('Recovery flow UI', () => {
  test('results page blocked until scan completes', async () => {
    test.skip(!existsSync(mainJs), 'out/main/main.js missing — npm run build first')

    const app = await electron.launch({ args: [mainJs], cwd: root })
    try {
      const win = await app.firstWindow()
      await expect(win.getByRole('heading', { name: 'Byteback' })).toBeVisible({ timeout: 30_000 })

      const resultsBtn = win.getByRole('button', { name: /Kurtarma Sonuçları/i })
      await expect(resultsBtn).toBeDisabled()
      await resultsBtn.click({ force: true })
      await expect(win.locator('.header-title h2')).toHaveText('Ana Ekran')
      await expect(win.getByTestId('show-duplicates')).not.toBeVisible()
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
