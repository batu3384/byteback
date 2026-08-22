import { describe, expect, it } from 'vitest'
import { isPausedScan, isUsableScan, scanPhaseFromState, scanProgressPercent, scanShowsMetadataResume, SCAN_STATUS } from './scan-session'
import type { ScanState } from './ipc-contract'

describe('scan-session', () => {
  it('paused only when status is 4', () => {
    const paused: ScanState = { id: 1, driveIndex: 0, scanType: 'deep', totalSectors: 100, scannedSectors: 50, status: SCAN_STATUS.paused }
    const running: ScanState = { ...paused, status: SCAN_STATUS.running }
    expect(isPausedScan(paused)).toBe(true)
    expect(isPausedScan(running)).toBe(false)
  })

  it('usable includes complete and paused', () => {
    expect(isUsableScan({ id: 1, driveIndex: 0, scanType: 'quick', totalSectors: 1, scannedSectors: 1, status: SCAN_STATUS.complete })).toBe(true)
    expect(isUsableScan({ id: 1, driveIndex: 0, scanType: 'deep', totalSectors: 100, scannedSectors: 10, status: SCAN_STATUS.paused })).toBe(true)
    expect(isUsableScan({ id: 1, driveIndex: 0, scanType: 'deep', totalSectors: 100, scannedSectors: 10, status: SCAN_STATUS.running })).toBe(false)
  })

  it('progress percent caps at 100', () => {
    expect(scanProgressPercent({ id: 1, driveIndex: 0, scanType: 'deep', totalSectors: 100, scannedSectors: 150, status: 4 })).toBe(100)
  })

  it('carve_only skips metadata resume hint and phase', () => {
    const paused: ScanState = {
      id: 1, driveIndex: 0, scanType: 'carve_only', totalSectors: 100, scannedSectors: 10,
      status: SCAN_STATUS.paused, metadataComplete: false,
    }
    expect(scanShowsMetadataResume(paused)).toBe(false)
    expect(scanPhaseFromState(paused)).toBe('carve_only')
  })
})
