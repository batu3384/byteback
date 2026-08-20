import { describe, it, expect } from 'vitest'
import { hexDataOrNull } from './hex-read'

describe('hexDataOrNull', () => {
  it('returns bytes for a clean read', () => {
    expect(hexDataOrNull({ success: true, data: [1, 2, 3] })).toEqual([1, 2, 3])
  })

  it('returns null when the read was zero-padded', () => {
    expect(hexDataOrNull({ success: true, paddedZeros: true, data: [0, 0] })).toBeNull()
  })

  it('returns null when success is false', () => {
    expect(hexDataOrNull({ success: false, data: [1] })).toBeNull()
  })
})
