import { test, expect, _electron as electron } from '@playwright/test'
import { existsSync } from 'node:fs'
import path from 'node:path'

const root = path.join(__dirname, '..')
const mainJs = path.join(root, 'out', 'main', 'main.js')

test('Wolf Recovery window shows', async () => {
  test.skip(!existsSync(mainJs), 'out/main/main.js missing — npm run build first')

  const app = await electron.launch({
    args: [mainJs],
    cwd: root,
  })
  try {
    const win = await app.firstWindow()
    await expect(win).toHaveTitle(/Wolf Recovery/)
    await expect(win.getByRole('heading', { name: 'Wolf Recovery' })).toBeVisible({
      timeout: 30_000,
    })
  } finally {
    await app.close()
  }
})
