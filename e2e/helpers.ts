import { expect, _electron as electron, type ElectronApplication, type Page } from '@playwright/test'
import { mkdtempSync, rmSync } from 'node:fs'
import { tmpdir } from 'node:os'
import path from 'node:path'

const root = path.join(__dirname, '..')
export const mainJs = path.join(root, 'out', 'main', 'main.js')

export interface LaunchedApp {
  app: ElectronApplication
  win: Page
  userData: string
}

/**
 * Launch the app with a fresh, isolated userData directory: DB, localStorage
 * and language choice cannot leak between tests.
 */
export async function launchApp(): Promise<LaunchedApp> {
  const userData = mkdtempSync(path.join(tmpdir(), 'byteback-e2e-'))
  const app = await electron.launch({
    args: [mainJs],
    cwd: root,
    env: { ...process.env, BYTEBACK_USER_DATA: userData, BYTEBACK_E2E: '1' },
  })
  const win = await app.firstWindow()
  await expect(win.getByRole('heading', { name: 'Byteback' })).toBeVisible({ timeout: 30_000 })
  return { app, win, userData }
}

export async function closeApp(launched: LaunchedApp): Promise<void> {
  await launched.app.close()
  rmSync(launched.userData, { recursive: true, force: true })
}
