import { test, expect, _electron as electron } from '@playwright/test'
import { existsSync } from 'node:fs'
import path from 'node:path'

const root = path.join(__dirname, '..')
const mainJs = path.join(root, 'out', 'main', 'main.js')

test('Byteback window shows', async () => {
  test.skip(!existsSync(mainJs), 'out/main/main.js missing — npm run build first')

  const app = await electron.launch({
    args: [mainJs],
    cwd: root,
  })
  try {
    const win = await app.firstWindow()
    await expect(win).toHaveTitle(/Byteback/)
    await expect(win.getByRole('heading', { name: 'Byteback' })).toBeVisible({
      timeout: 30_000,
    })
  } finally {
    await app.close()
  }
})

test('scan profile legend and mode buttons', async () => {
  test.skip(!existsSync(mainJs), 'out/main/main.js missing — npm run build first')

  const app = await electron.launch({
    args: [mainJs],
    cwd: root,
  })
  try {
    const win = await app.firstWindow()
    await expect(win.getByRole('heading', { name: 'Byteback' })).toBeVisible({
      timeout: 30_000,
    })

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
    await app.close()
  }
})
