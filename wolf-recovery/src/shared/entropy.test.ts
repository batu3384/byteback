import { describe, it, expect } from 'vitest'
import { calculateEntropy, classifyEntropy } from './entropy'

describe('calculateEntropy', () => {
  it('returns 0 for an empty buffer', () => {
    expect(calculateEntropy([])).toBe(0)
  })

  it('returns 0 for a buffer of a single repeated value (no information)', () => {
    const zeros = new Array(1024).fill(0)
    expect(calculateEntropy(zeros)).toBe(0)

    const allSame = new Array(512).fill(0x42)
    expect(calculateEntropy(allSame)).toBe(0)
  })

  it('approaches 1 bit for a 50/50 split between two values', () => {
    // A two-symbol uniform stream has exactly 1.0 bit of entropy.
    const buf: number[] = []
    for (let i = 0; i < 1000; i++) buf.push(i % 2 === 0 ? 0x00 : 0xff)
    expect(calculateEntropy(buf)).toBeCloseTo(1.0, 6)
  })

  it('approaches 8 bits for a uniform distribution across all 256 byte values', () => {
    // Build one occurrence of each byte value, repeated to smooth rounding.
    const buf: number[] = []
    for (let rep = 0; rep < 100; rep++) {
      for (let v = 0; v < 256; v++) buf.push(v)
    }
    expect(calculateEntropy(buf)).toBeCloseTo(8.0, 6)
  })

  it('is bounded within [0, 8] for random-ish data', () => {
    const pseudo = Array.from({ length: 4096 }, (_, i) => (i * 1103515245 + 12345) & 0xff)
    const e = calculateEntropy(pseudo)
    expect(e).toBeGreaterThanOrEqual(0)
    expect(e).toBeLessThanOrEqual(8)
  })

  it('masks out-of-range byte values instead of producing NaN', () => {
    // Defensive: the IPC layer should already supply 0..255, but we must not
    // blow up if a negative or >255 sneaks in.
    const e = calculateEntropy([-1, 256, 255, 0])
    expect(Number.isFinite(e)).toBe(true)
  })

  it('accepts a Uint8Array as well as number[]', () => {
    const u = new Uint8Array([0, 1, 2, 3, 0, 1, 2, 3])
    const n = [0, 1, 2, 3, 0, 1, 2, 3]
    expect(calculateEntropy(u)).toBeCloseTo(calculateEntropy(n), 6)
  })
})

describe('classifyEntropy', () => {
  it('classifies zeroed/constant data as low', () => {
    expect(classifyEntropy(0)).toBe('low')
    expect(classifyEntropy(2.0)).toBe('low')
    expect(classifyEntropy(4.5)).toBe('low') // boundary is exclusive
  })

  it('classifies structured data as mid', () => {
    expect(classifyEntropy(4.5001)).toBe('mid')
    expect(classifyEntropy(6.0)).toBe('mid')
    expect(classifyEntropy(7.0)).toBe('mid') // boundary is exclusive
  })

  it('classifies encrypted/compressed data as high', () => {
    expect(classifyEntropy(7.0001)).toBe('high')
    expect(classifyEntropy(8.0)).toBe('high')
  })
})
