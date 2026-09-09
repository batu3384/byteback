import { describe, expect, it } from 'vitest'
import { DEFAULT_ROW_HEIGHT, ROW_OVERSCAN, computeVirtualWindow, estimateRowHeight } from './virtual-rows'

describe('computeVirtualWindow', () => {
  it('top of list renders the first band plus overscan', () => {
    expect(computeVirtualWindow({ scrollTop: 0, viewportHeight: 400, rowHeight: 40, totalRows: 1000 }))
      .toEqual({ start: 0, end: 10 + ROW_OVERSCAN })
  })

  it('bottom clamp never exceeds totalRows', () => {
    const w = computeVirtualWindow({ scrollTop: 40 * 999, viewportHeight: 400, rowHeight: 40, totalRows: 1000 })
    expect(w.end).toBe(1000)
    // firstVisible = 999 (the last row); start = 999 - overscan.
    expect(w.start).toBe(999 - ROW_OVERSCAN)
  })

  it('a scroll far past the end clamps to the last row (start <= end)', () => {
    const w = computeVirtualWindow({ scrollTop: 40 * 99999, viewportHeight: 400, rowHeight: 40, totalRows: 10 })
    expect(w.start).toBe(9)
    expect(w.end).toBe(10)
  })

  it('totalRows smaller than the viewport renders everything (no overscan past end)', () => {
    expect(computeVirtualWindow({ scrollTop: 0, viewportHeight: 4000, rowHeight: 40, totalRows: 5 }))
      .toEqual({ start: 0, end: 5 })
  })

  it('overscan extends the band on both sides', () => {
    const w = computeVirtualWindow({ scrollTop: 40 * 50, viewportHeight: 200, rowHeight: 40, totalRows: 1000, overscan: 5 })
    expect(w.start).toBe(50 - 5)
    expect(w.end).toBe(55 + 5)
  })

  it('fractional scrollTop floors to the row boundary', () => {
    const w = computeVirtualWindow({ scrollTop: 40 * 50 + 39.9, viewportHeight: 80, rowHeight: 40, totalRows: 1000 })
    // 2039.9 / 40 -> floor 50th row visible; ceil(80/40)=2 rows viewport.
    expect(w.start).toBe(50 - ROW_OVERSCAN)
    expect(w.end).toBe(52 + ROW_OVERSCAN)
  })

  it('degenerate inputs degrade to a safe empty/at-top window', () => {
    expect(computeVirtualWindow({ scrollTop: 0, viewportHeight: 0, rowHeight: 40, totalRows: 0 })).toEqual({ start: 0, end: 0 })
    expect(computeVirtualWindow({ scrollTop: -500, viewportHeight: 400, rowHeight: 40, totalRows: 100 }).start).toBe(0)
    // jsdom reports clientHeight 0 and a garbage rowHeight must not NaN.
    const w = computeVirtualWindow({ scrollTop: 0, viewportHeight: 0, rowHeight: 0, totalRows: 50 })
    expect(w).toEqual({ start: 0, end: Math.min(50, ROW_OVERSCAN) })
    expect(Number.isFinite(w.start) && Number.isFinite(w.end)).toBe(true)
  })
})

describe('estimateRowHeight', () => {
  it('accepts sane measurements and falls back to 40 otherwise', () => {
    expect(estimateRowHeight(33.5)).toBe(33.5)
    expect(estimateRowHeight(0)).toBe(DEFAULT_ROW_HEIGHT)
    expect(estimateRowHeight(null)).toBe(DEFAULT_ROW_HEIGHT)
    expect(estimateRowHeight(undefined)).toBe(DEFAULT_ROW_HEIGHT)
    expect(estimateRowHeight(9999)).toBe(DEFAULT_ROW_HEIGHT)
  })
})
