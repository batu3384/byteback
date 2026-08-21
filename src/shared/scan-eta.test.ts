import { describe, expect, it } from 'vitest'
import { etaFromMonotonicWindow } from './scan-eta'

describe('scan-eta', () => {
  it('ignores a backward work tick so ETA does not invert', () => {
    const t0 = 1_000_000
    const a = etaFromMonotonicWindow([], 10, 100, t0)
    const b = etaFromMonotonicWindow(a.history, 80, 100, t0 + 1000)
    const c = etaFromMonotonicWindow(b.history, 20, 100, t0 + 2000, 5000, b.speed)
    expect(c.history[c.history.length - 1].work).toBe(80)
    expect(c.etaSeconds).toBeGreaterThan(0)
    expect(c.speed).toBeGreaterThan(0)
  })

  it('returns no ETA when complete', () => {
    const r = etaFromMonotonicWindow([{ timestamp: 0, work: 50 }], 100, 100, 1000)
    expect(r.etaSeconds).toBe(-1)
  })
})
