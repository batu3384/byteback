export interface SectorReadResult {
  success: boolean
  paddedZeros?: boolean
  data?: ArrayLike<number> | null
}

/** Hex UI shows a sector only when the read is complete and not zero-padded. */
export function hexDataOrNull(res: SectorReadResult | null | undefined): number[] | null {
  if (!res || !res.success || res.paddedZeros || !res.data) return null
  const out: number[] = []
  const data = res.data
  const n = data.length
  if (n <= 0) return null
  for (let i = 0; i < n; i++) out.push(data[i])
  return out
}
