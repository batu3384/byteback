import { test, expect, _electron as electron } from '@playwright/test'
import { existsSync } from 'node:fs'
import path from 'node:path'

const root = path.join(__dirname, '..')
const mainJs = path.join(root, 'out', 'main', 'main.js')

test('report generate button disabled without scan', async () => {
  test.skip(!existsSync(mainJs), 'out/main/main.js missing — npm run build first')

  const app = await electron.launch({ args: [mainJs], cwd: root })
  try {
    const win = await app.firstWindow()
    await expect(win.getByRole('heading', { name: 'Byteback' })).toBeVisible({ timeout: 30_000 })

    await expect(win.getByTestId('nav-report')).toBeDisabled()
  } finally {
    await app.close()
  }
})
