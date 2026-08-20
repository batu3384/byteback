/** Per-segment EWF1 table offsets are uint32 — writer rotates .E02 at this size. */
export const EWF_SEGMENT_MAX_BYTES = 0xffffffff

export function ewfWillRotateSegments(sizeBytes: number): boolean {
  return sizeBytes > EWF_SEGMENT_MAX_BYTES
}

/** @deprecated alias — rotation is supported; kept for existing imports */
export const EWF_SINGLE_SEGMENT_MAX_BYTES = EWF_SEGMENT_MAX_BYTES
export const ewfNeedsSegmentWarning = ewfWillRotateSegments
