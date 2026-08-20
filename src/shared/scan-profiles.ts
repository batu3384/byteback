/** Scan profile labels shown in UI — keep in sync with native scanType strings. */

export type ScanProfile = 'quick' | 'deep' | 'full_carve'

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
}

export function scanProfileLabel(scanType: string): string {
  const p = SCAN_PROFILES[scanType as ScanProfile]
  return p ? `${p.label} — ${p.short}` : scanType
}

export function scanProfileDetail(scanType: string): string {
  const p = SCAN_PROFILES[scanType as ScanProfile]
  return p?.detail ?? scanType
}
