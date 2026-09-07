import { describe, it, expect, vi } from 'vitest'

vi.mock('electron', () => ({
  ipcMain: { handle: vi.fn(), on: vi.fn() },
  app: { getPath: () => '.', isPackaged: true },
  BrowserWindow: { getAllWindows: () => [], getFocusedWindow: () => null },
  dialog: {},
  powerSaveBlocker: { start: () => 1, stop: () => {}, isStarted: () => false },
}))

import { clampInt, scanProgressPayload } from './ipc-handlers'

// Main once dropped phaseCurrent/phaseTotal from native progress events, leaving
// the ScanView phase % dead despite the renderer reading them (bridge_scan.cpp
// emits both). The payload must pass the whole shape through with scanId stamped.
describe('scanProgressPayload', () => {
  it('stamps scanId and forwards phase-local counters', () => {
    const out = scanProgressPayload(7, {
      type: 'progress',
      current: 10,
      total: 100,
      phase: 'carve',
      phaseCurrent: 3,
      phaseTotal: 40,
      badSectors: [5, 9],
    })
    expect(out).toEqual({
      scanId: 7,
      current: 10,
      total: 100,
      phase: 'carve',
      phaseCurrent: 3,
      phaseTotal: 40,
      badSectors: [5, 9],
    })
  })

  it('keeps absent optional fields absent', () => {
    const out = scanProgressPayload(-1, { type: 'progress', current: 1, total: 2 })
    expect(out).toEqual({ scanId: -1, current: 1, total: 2, badSectors: undefined, phase: undefined, phaseCurrent: undefined, phaseTotal: undefined })
  })
})

describe('clampInt', () => {
  it('falls back on non-finite input and clamps/floors the rest', () => {
    expect(clampInt(NaN, 0, 10, 4)).toBe(4)
    expect(clampInt(Infinity, 0, 10, 4)).toBe(4)
    expect(clampInt(-Infinity, 0, 10, 4)).toBe(4)
    expect(clampInt(-5, 0, 10, 4)).toBe(0)
    expect(clampInt(3.7, 0, 10, 4)).toBe(3)
    expect(clampInt('12', 0, 10, 4)).toBe(10)
    expect(clampInt(undefined, 1, 5000, 500)).toBe(500)
  })
})
