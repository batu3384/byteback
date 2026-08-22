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
    detail: 'Metadata + yalnızca boş (unallocated) alanda imza carve. Önerilen mod.',
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
