import { describe, it, expect } from 'vitest'
import { EWF_SEGMENT_MAX_BYTES, ewfWillRotateSegments } from './ewf-limits'

describe('ewfWillRotateSegments', () => {
  it('is false at and below the uint32 chunk-table ceiling', () => {
    expect(ewfWillRotateSegments(0)).toBe(false)
    expect(ewfWillRotateSegments(EWF_SEGMENT_MAX_BYTES)).toBe(false)
  })

  it('is true when image bytes exceed 4 GiB minus 1', () => {
    expect(ewfWillRotateSegments(EWF_SEGMENT_MAX_BYTES + 1)).toBe(true)
  })
})
