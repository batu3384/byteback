/** Single-segment EWF1 table entries are uint32 — chunk data cannot exceed this. */
export const EWF_SINGLE_SEGMENT_MAX_BYTES = 0xffffffff

export function ewfNeedsSegmentWarning(sizeBytes: number): boolean {
  return sizeBytes > EWF_SINGLE_SEGMENT_MAX_BYTES
}
