import { appendFileSync, mkdirSync, readFileSync, existsSync } from 'fs'
import { dirname, join } from 'path'
import { powerSaveBlocker } from 'electron'
import { summarizeSessionLines } from '../shared/session-log'

let logPath = ''
let scanLive = false
let lastProgressLogMs = 0
let sleepBlocker: number | null = null

export function isScanLive(): boolean {
  return scanLive
}

function setSleepBlocked(block: boolean): void {
  try {
    if (block) {
      if (sleepBlocker != null && powerSaveBlocker.isStarted(sleepBlocker)) return
      sleepBlocker = powerSaveBlocker.start('prevent-app-suspension')
      appendSessionLog('SLEEP_BLOCK', `id=${sleepBlocker}`)
      return
    }
    if (sleepBlocker != null && powerSaveBlocker.isStarted(sleepBlocker)) {
      powerSaveBlocker.stop(sleepBlocker)
      appendSessionLog('SLEEP_UNBLOCK', `id=${sleepBlocker}`)
    }
    sleepBlocker = null
  } catch (e) {
    appendSessionLog('SLEEP_BLOCK', `failed ${e instanceof Error ? e.message : String(e)}`)
  }
}

export function setScanLive(live: boolean): void {
  scanLive = live
  if (!live) lastProgressLogMs = 0
  setSleepBlocked(live)
}

export function sessionLogPath(): string {
  return logPath
}

export function initSessionLog(userDataDir: string): string {
  logPath = join(userDataDir, 'session.log')
  mkdirSync(userDataDir, { recursive: true })
  appendSessionLog('APP_START', `pid=${process.pid}`)
  return logPath
}

export function appendSessionLog(event: string, detail = ''): void {
  const path = logPath || join(process.env.APPDATA || process.env.HOME || '.', 'byteback', 'session.log')
  const line = `${new Date().toISOString()} ${event}${detail ? ` ${detail}` : ''}\n`
  try {
    mkdirSync(dirname(path), { recursive: true })
    appendFileSync(path, line)
  } catch {
    /* disk full / locked — do not throw from logging */
  }
}

export function appendProgressLog(current: number, total: number, phase?: string): void {
  const now = Date.now()
  if (now - lastProgressLogMs < 15_000) return
  lastProgressLogMs = now
  appendSessionLog('SCAN_PROGRESS', `current=${current} total=${total} phase=${phase ?? ''}`)
}

export function readSessionLog(maxLines = 80): { path: string; lines: string[]; summary: string } {
  const path = logPath
  if (!path || !existsSync(path)) {
    return { path: path || '', lines: [], summary: 'Günlük dosyası yok.' }
  }
  let text = ''
  try {
    text = readFileSync(path, 'utf8')
  } catch {
    return { path, lines: [], summary: 'Günlük okunamadı.' }
  }
  const lines = text.split(/\r?\n/).filter(Boolean)
  const tail = lines.slice(Math.max(0, lines.length - maxLines))
  return { path, lines: tail, summary: summarizeSessionLines(tail) }
}
