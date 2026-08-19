import { describe, it, expect } from 'vitest'
import { EWF_SINGLE_SEGMENT_MAX_BYTES, ewfNeedsSegmentWarning } from './ewf-limits'

describe('ewfNeedsSegmentWarning', () => {
  it('is false at and below the uint32 chunk-table ceiling', () => {
    expect(ewfNeedsSegmentWarning(0)).toBe(false)
    expect(ewfNeedsSegmentWarning(EWF_SINGLE_SEGMENT_MAX_BYTES)).toBe(false)
  })

  it('is true when image bytes exceed 4 GiB minus 1', () => {
    expect(ewfNeedsSegmentWarning(EWF_SINGLE_SEGMENT_MAX_BYTES + 1)).toBe(true)
  })
})
