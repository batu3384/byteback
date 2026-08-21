import { describe, expect, it } from 'vitest'
import { etaFromMonotonicWindow, formatEtaClock, scanPhaseLabel, scanStepIndex } from './scan-eta'

describe('scan-eta', () => {
  it('ignores a backward work tick so ETA does not invert', () => {
    const t0 = 1_000_000
    const a = etaFromMonotonicWindow([], 10, 10_000, t0)
    const b = etaFromMonotonicWindow(a.history, 80, 10_000, t0 + 1000)
    const c = etaFromMonotonicWindow(b.history, 20, 10_000, t0 + 2000, 5000, b.speed)
    expect(c.history[c.history.length - 1].work).toBe(80)
    expect(c.etaSeconds).toBeGreaterThan(0)
    expect(c.speed).toBeGreaterThan(0)
  })

  it('returns no ETA when complete', () => {
    const r = etaFromMonotonicWindow([{ timestamp: 0, work: 50 }], 100, 100, 1000)
    expect(r.etaSeconds).toBe(-1)
  })

  it('resets speed on a budget jump so metadata→carve does not look instant', () => {
    const t0 = 1_000_000
    const a = etaFromMonotonicWindow([], 10, 1_000_000, t0)
    const jumped = etaFromMonotonicWindow(a.history, 750_000, 1_000_000, t0 + 1000)
    expect(jumped.history).toHaveLength(1)
    expect(jumped.history[0].work).toBe(750_000)
    expect(jumped.speed).toBe(0)
    expect(jumped.etaSeconds).toBe(-1)
  })

  it('marks stall when work does not move', () => {
    const t0 = 1_000_000
    const a = etaFromMonotonicWindow([], 100, 200, t0)
    const b = etaFromMonotonicWindow(a.history, 100, 200, t0 + 9000, 30_000, 0, 0.15, 8000)
    expect(b.stalled).toBe(true)
    expect(b.etaSeconds).toBe(-1)
  })

  it('formats unknown ETA as dash', () => {
    expect(formatEtaClock(-1)).toBe('—')
    expect(scanPhaseLabel('carve')).toContain('Oyma')
  })

  it('uses a single step for quick scan and two for deep carve', () => {
    expect(scanStepIndex('metadata', 'quick')).toEqual({ step: 1, of: 1 })
    expect(scanStepIndex('metadata', 'deep')).toEqual({ step: 1, of: 2 })
    expect(scanStepIndex('carve', 'deep')).toEqual({ step: 2, of: 2 })
    expect(scanStepIndex('carve', 'full_carve')).toEqual({ step: 2, of: 2 })
  })
})
