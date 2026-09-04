/** Pages and actions that require a scan session persisted in SQLite. */

import { isCompleteScan, isUsableScan, SCAN_STATUS } from './scan-session'
import type { ScanState } from './ipc-contract'

export const SCAN_DEPENDENT_PAGES = ['results', 'search', 'timeline', 'report'] as const
export const DISK_BUSY_PAGES = ['hex', 'imager', 'shredder'] as const

export type ScanDependentPage = (typeof SCAN_DEPENDENT_PAGES)[number]

export function isScanDependentPage(page: string): page is ScanDependentPage {
  return (SCAN_DEPENDENT_PAGES as readonly string[]).includes(page)
}

export type DiskBusyPage = (typeof DISK_BUSY_PAGES)[number]

export function isDiskBusyPage(page: string): page is DiskBusyPage {
  return (DISK_BUSY_PAGES as readonly string[]).includes(page)
}

export function hasValidScanId(scanId: number): boolean {
  return typeof scanId === 'number' && scanId > 0
}

/** Results / search / timeline: paused or complete session is enough. */
export function hasUsableScanSession(scanId: number, state?: ScanState | null): boolean {
  if (!hasValidScanId(scanId)) return false
  if (state && state.id === scanId) return isUsableScan(state)
  return true
}

/** Adli rapor: only status=complete (1). Paused scans stay locked. */
export function canGenerateReport(scanId: number, state?: ScanState | null): boolean {
  if (!hasValidScanId(scanId)) return false
  if (state && state.id === scanId) return isCompleteScan(state)
  return false
}

/**
 * CA-017: the busy gate is a state machine, not display text. Renderer keeps
 * this phase in App state; strings are derived from it, never the reverse.
 */
export type ScanPhase =
  | 'idle'
  | 'starting'
  | 'running'
  | 'stopping'
  | 'complete'
  | 'paused'
  | 'stopped'
  | 'failed'

export function isLiveScanPhase(phase: ScanPhase): boolean {
  return phase === 'starting' || phase === 'running' || phase === 'stopping'
}

/** Maps a native scans.status code to a renderer phase. -1 = unknown/still running. */
export function scanPhaseFromStatusCode(status: number): ScanPhase {
  if (status === SCAN_STATUS.complete) return 'complete'
  if (status === SCAN_STATUS.stopped) return 'stopped'
  if (status === SCAN_STATUS.paused) return 'paused'
  if (status === SCAN_STATUS.failed) return 'failed'
  return 'running'
}

export function diskBusyMessage(raw?: string): string | undefined {
  if (!raw) return undefined
  if (raw.includes('Another disk operation is already running')) {
    return 'Hex, imaj, imha, önizleme ve kurtarma tarama bitene kadar kapalı. Sonuç listesine geçebilirsin.'
  }
  return raw
}

export { SCAN_STATUS }
