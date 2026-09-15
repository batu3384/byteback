/** Scan profile labels shown in UI — keep in sync with native scanType strings. */

export type ScanProfile = 'quick' | 'deep' | 'full_carve' | 'carve_only'

export const SCAN_PROFILES: Record<ScanProfile, { label: string; short: string; detail: string }> = {
  quick: {
    label: 'Hızlı',
    short: 'Metadata',
    detail: 'Dosya sistemi metadata ($MFT, FAT, ext4, HFS, APFS, VSS). NTFS orphan carve kapalı.',
  },
  deep: {
    label: 'Derin',
    short: 'Metadata + boş alan carve',
    detail: 'Metadata + NTFS/FAT/ext4 boş alanda imza carve. APFS/HFS/ReFS bitmap yoksa tüm bölüm (carve_fallback). Önerilen mod.',
  },
  full_carve: {
    label: 'Tam disk carve',
    short: 'Metadata + tüm alan carve',
    detail: 'Metadata + allocated ve boş tüm alanda imza carve. Çok yavaş; eski davranış.',
  },
  carve_only: {
    label: 'Yalnızca carve',
    short: 'PhotoRec tarzı imza taraması',
    detail: 'Dosya sistemi metadata atlanır; tüm seçili alanda yalnızca imza carve. Bozuk/formatlı diskler için.',
  },
}

export function scanProfileLabel(scanType: string): string {
  const p = SCAN_PROFILES[scanType as ScanProfile]
  return p ? `${p.label} — ${p.short}` : scanType
}

export function scanProfileDetail(scanType: string): string {
  const p = SCAN_PROFILES[scanType as ScanProfile]
  return p?.detail ?? scanType
}

/** Deep / carve profiles on SSD require explicit TRIM acknowledgment. */
export function scanNeedsSsdDeepAck(scanType: string): boolean {
  return scanType === 'deep' || scanType === 'full_carve' || scanType === 'carve_only'
}

/** Wear-leveling risk: known SSD, SMART saying SSD, or SMART unread.
 *  Unread SMART is not "not an SSD" — skipping the TRIM ack would be silent. */
export function mediaNeedsTrimAck(
  scanType: string,
  driveType: string | undefined,
  smart: { unread?: boolean; saysSsd?: boolean } = {},
): boolean {
  if (!scanNeedsSsdDeepAck(scanType)) return false
  if (driveType === 'SSD' || smart.saysSsd || smart.unread) return true
  return false
}

/** Map a SMART payload to TRIM-ack signals. Missing health or seek-penalty
 *  data is unread, not "confirmed HDD". */
export function smartTrimSignals(
  s: { isValid?: boolean; isSsd?: boolean; seekPenaltyKnown?: boolean } | null | undefined,
  threw = false,
): { unread: boolean; saysSsd: boolean } {
  if (threw || !s) return { unread: true, saysSsd: false }
  return {
    saysSsd: !!s.isSsd,
    unread: !s.isValid || s.seekPenaltyKnown !== true,
  }
}
