import { test, expect, _electron as electron } from '@playwright/test'
import path from 'node:path'

const root = path.join(__dirname, '..')
const mainJs = path.join(root, 'out', 'main', 'main.js')

test('scan-dependent sidebar items disabled without completed scan', async () => {

  const app = await electron.launch({ args: [mainJs], cwd: root })
  try {
    const win = await app.firstWindow()
    await expect(win.getByRole('heading', { name: 'Byteback' })).toBeVisible({ timeout: 30_000 })

    await expect(win.getByTestId('nav-report')).toBeDisabled()
    await expect(win.getByTestId('nav-results')).toBeDisabled()
    await expect(win.getByTestId('nav-search')).toBeDisabled()
    await expect(win.getByTestId('nav-timeline')).toBeDisabled()
  } finally {
    await app.close()
  }
})

test('scan-dependent nav stays on dashboard when report is forced', async () => {

  const app = await electron.launch({ args: [mainJs], cwd: root })
  try {
    const win = await app.firstWindow()
    await expect(win.getByRole('heading', { name: 'Byteback' })).toBeVisible({ timeout: 30_000 })

    const reportBtn = win.getByTestId('nav-report')
    await expect(reportBtn).toBeDisabled()
    await reportBtn.click({ force: true })
    await expect(win.locator('.header-title h2')).toHaveText('Ana Ekran')
  } finally {
    await app.close()
  }
})
