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
 * Environment for a spawned app: isolated userData + e2e flag.
 * ELECTRON_RUN_AS_NODE is stripped — if it leaks in from a CI/dev shell,
 * electron.exe runs as plain node and every launch "fails to launch".
 */
export function e2eEnv(userData: string): Record<string, string> {
  const env: Record<string, string> = {}
  for (const [key, value] of Object.entries(process.env)) {
    if (value !== undefined) env[key] = value
  }
  env.BYTEBACK_USER_DATA = userData
  env.BYTEBACK_E2E = '1'
  delete env.ELECTRON_RUN_AS_NODE
  return env
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
    env: e2eEnv(userData),
  })
  const win = await app.firstWindow()
  await expect(win.getByRole('heading', { name: 'Byteback' })).toBeVisible({ timeout: 30_000 })
  return { app, win, userData }
}

export async function closeApp(launched: LaunchedApp): Promise<void> {
  await launched.app.close()
  rmSync(launched.userData, { recursive: true, force: true })
}
