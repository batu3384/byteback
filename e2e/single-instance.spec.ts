import { test, expect, _electron as electron, type ElectronApplication } from '@playwright/test'
import path from 'node:path'
import { launchApp, closeApp, mainJs } from './helpers'

const root = path.join(__dirname, '..')

// Forensic exclusivity: a second instance pointed at the same userData must
// lose the single-instance lock and exit, leaving the first window alive.
test('second instance exits on the single-instance lock', async () => {
  const launched = await launchApp()
  const { win, userData } = launched
  try {
    let second: ElectronApplication | undefined
    try {
      second = await electron.launch({
        args: [mainJs],
        cwd: root,
        env: { ...process.env, BYTEBACK_USER_DATA: userData, BYTEBACK_E2E: '1' },
      })
    } catch {
      // The quitting instance can drop the debug endpoint mid-launch — that
      // still proves it did not stay alive.
    }
    if (second) {
      const proc = second.process()
      await expect
        .poll(() => proc.exitCode !== null || proc.signalCode !== null, { timeout: 15_000 })
        .toBe(true)
      await second.close().catch(() => {})
    }

    // First instance keeps a working window.
    await expect(win.getByRole('heading', { name: 'Byteback' })).toBeVisible()
  } finally {
    await closeApp(launched)
  }
})
