const DISCOVERY_ONLY = new Set([
  // Keep in sync with native/include/scan/discovery_sources.h (isDiscoverySourceName).
  'apfs_container',
  'apfs_volume',
  'apfs_file',
  'bitlocker_detect',
  'bitlocker_fve',
  'vss_unbound',
  'vss_bind',
  'vss_snapshot',
  'hfs_limit',
  'hfs_vh',
  'hfs_catalog',
  'usn_journal',
  'ntfs_logfile',
  'ntfs_logfile_restart',
  'ntfs_recycle_meta',
  'ntfs_i30',
  'Folder',
  'refs_volume',
])

export function isDuplicateSource(source?: string): boolean {
  return source === 'carver_duplicate'
}

export function isDiscoveryOnlySource(source?: string): boolean {
  if (!source) return false
  return DISCOVERY_ONLY.has(source)
}

export function isRecoverableListSource(source?: string): boolean {
  return !isDiscoveryOnlySource(source) && !isDuplicateSource(source)
}

export function canRecoverSource(source?: string, hasRuns?: boolean): boolean {
  if (isDiscoveryOnlySource(source)) return false
  if (isDuplicateSource(source)) return false
  if (source === 'apfs_extent') return !!hasRuns
  if (source === 'ntfs_thumbcache') return true
  return true
}

export function sourceDisplayLabel(source?: string): string {
  if (source === 'apfs_container' || source === 'apfs_volume') {
    return 'APFS keşif (nx_fs_oid + omap paddr + APSB)'
  }
  if (source === 'apfs_file') return 'APFS katalog adı (extent yok, kurtarılamaz)'
  if (source === 'apfs_extent') return 'APFS extent (kurtarılabilir)'
  if (source === 'hfs_limit') return 'HFS katalog tavanı (test/limit)'
  if (source === 'vss_unbound') return 'VSS bağlanmadı'
  if (source === 'vss_bind') return 'VSS bağlandı (serial+boyut)'
  if (source === 'vss_snapshot') return 'VSS snapshot (üstveri)'
  if (source === 'vss_ntfs') return 'VSS NTFS'
  if (source === 'vss_fat') return 'VSS FAT'
  if (source === 'bitlocker_detect') return 'BitLocker (anahtar yok, şifre kırma yok)'
  if (source === 'bitlocker_fve') return 'BitLocker FVE (kayıt decrypt değil)'
  if (source === 'ntfs_mft_logfile') return 'NTFS MFT (LogFile doğrulandı)'
  if (source === 'ntfs_recycle') return 'Geri Dönüşüm Kutusu ($R)'
  if (source === 'ntfs_recycle_meta') return 'Geri Dönüşüm Kutusu ($I, yalnızca ad)'
  if (source === 'ntfs_i30') return 'NTFS $I30 slack (yalnızca ad, kurtarılamaz)'
  if (source === 'ntfs_thumbcache') return 'NTFS thumbcache (gömülü JPEG)'
  if (source === 'usn_journal') return 'USN zaman çizelgesi'
  if (source === 'ntfs_logfile') return 'LogFile ipucu (kurtarılamaz)'
  if (source === 'carver_duplicate') return 'Carve tekrarı (MFT ile çakışıyor)'
  return source ?? ''
}
