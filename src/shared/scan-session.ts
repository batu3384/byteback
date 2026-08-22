import type { ScanState } from './ipc-contract'

/** Scan row status in SQLite (native byteback_db.h). */
export const SCAN_STATUS = {
  running: 0,
  complete: 1,
  stopped: 2,
  failed: 3,
  paused: 4,
} as const

export function isPausedScan(state?: ScanState | null): boolean {
  return state != null && state.status === SCAN_STATUS.paused
}

export function isCompleteScan(state?: ScanState | null): boolean {
  return state != null && state.status === SCAN_STATUS.complete
}

export function isUsableScan(state?: ScanState | null): boolean {
  return isPausedScan(state) || isCompleteScan(state)
}

export function scanProgressPercent(state: ScanState): number {
  if (!state.totalSectors) return 0
  return Math.min(100, Math.floor((state.scannedSectors / state.totalSectors) * 100))
}

export function scanPhaseFromState(state: ScanState): 'metadata' | 'carve' | 'carve_only' {
  if (state.scanType === 'carve_only') return 'carve_only'
  return state.metadataComplete ? 'carve' : 'metadata'
}

export function scanShowsMetadataResume(state: ScanState): boolean {
  if (state.scanType === 'quick' || state.scanType === 'carve_only') return false
  return !state.metadataComplete
}
