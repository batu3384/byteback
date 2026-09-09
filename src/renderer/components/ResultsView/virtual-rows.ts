// FAZ 1.3a — pure virtualization math for the results table. Keeping the
// window arithmetic here (not in the component) makes the edge cases unit
// testable without a DOM.

/** Fallback row height when the probe measurement is unavailable (jsdom
 *  reports 0; a hidden table has no layout). Matches the CSS row pacing. */
export const DEFAULT_ROW_HEIGHT = 40

/** Rows rendered above/below the visible band so small scrolls do not blank. */
export const ROW_OVERSCAN = 3

export interface VirtualWindowArgs {
  scrollTop: number
  viewportHeight: number
  rowHeight: number
  totalRows: number
  overscan?: number
}

export interface VirtualWindow {
  /** First rendered row index (inclusive). */
  start: number
  /** One past the last rendered row index (exclusive). */
  end: number
}

export function computeVirtualWindow({
  scrollTop,
  viewportHeight,
  rowHeight,
  totalRows,
  overscan = ROW_OVERSCAN,
}: VirtualWindowArgs): VirtualWindow {
  if (!Number.isFinite(totalRows) || totalRows <= 0) return { start: 0, end: 0 }
  const rh = Number.isFinite(rowHeight) && rowHeight > 0 ? rowHeight : DEFAULT_ROW_HEIGHT
  // jsdom has no layout (viewport 0) and rubber-band scrolls can go negative:
  // both must degrade to "window at the top", never NaN indexes.
  const top = Number.isFinite(scrollTop) && scrollTop > 0 ? Math.floor(scrollTop) : 0
  const vh = Number.isFinite(viewportHeight) && viewportHeight > 0 ? viewportHeight : 0
  const ov = Number.isFinite(overscan) && overscan > 0 ? Math.floor(overscan) : 0
  const firstVisible = Math.floor(top / rh)
  const visibleCount = Math.ceil(vh / rh)
  // A scroll far past the last row clamps to the final row instead of
  // producing start > end (which would render nothing but wrong spacers).
  const start = Math.min(Math.max(0, firstVisible - ov), totalRows - 1)
  const end = Math.min(totalRows, firstVisible + visibleCount + ov)
  return { start: Math.min(start, end), end }
}

/** Measured row height guard: accept sane pixel measurements, fall back to
 *  DEFAULT_ROW_HEIGHT for 0/negative/absurd values (jsdom, collapsed tables). */
export function estimateRowHeight(measured: number | null | undefined): number {
  return typeof measured === 'number' && Number.isFinite(measured) && measured >= 8 && measured <= 200
    ? measured
    : DEFAULT_ROW_HEIGHT
}
