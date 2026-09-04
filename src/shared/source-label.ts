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

/** i18n key for sources with a curated label; callers translate via t().
 *  Unknown sources return the raw id (locale-neutral technical name). */
const KEY_BY_SOURCE: Record<string, string> = {
  apfs_container: 'source.apfs_container',
  apfs_volume: 'source.apfs_container',
  apfs_file: 'source.apfs_file',
  apfs_extent: 'source.apfs_extent',
  hfs_limit: 'source.hfs_limit',
  vss_unbound: 'source.vss_unbound',
  vss_bind: 'source.vss_bind',
  vss_snapshot: 'source.vss_snapshot',
  vss_ntfs: 'source.vss_ntfs',
  vss_fat: 'source.vss_fat',
  bitlocker_detect: 'source.bitlocker_detect',
  bitlocker_fve: 'source.bitlocker_fve',
  ntfs_mft_logfile: 'source.ntfs_mft_logfile',
  ntfs_mft_usn: 'source.ntfs_mft_usn',
  ntfs_recycle: 'source.ntfs_recycle',
  ntfs_recycle_meta: 'source.ntfs_recycle_meta',
  ntfs_i30: 'source.ntfs_i30',
  ntfs_thumbcache: 'source.ntfs_thumbcache',
  usn_journal: 'source.usn_journal',
  ntfs_logfile: 'source.ntfs_logfile',
  carver_duplicate: 'source.carver_duplicate',
}

export function sourceLabelKey(source?: string): string {
  if (!source) return ''
  const key = KEY_BY_SOURCE[source]
  return key ?? source
}

/** Convenience: translate with the passed t(). */
export function localizeSourceLabel(source: string | undefined, t: (key: string) => string): string {
  const key = sourceLabelKey(source)
  return key.startsWith('source.') ? t(key) : key
}
