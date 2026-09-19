const DISCOVERY_ONLY = new Set([
  // Keep in sync with native/include/scan/discovery_sources.h (isDiscoverySourceName).
  'apfs_container',
  'apfs_volume',
  'apfs_file',
  'apfs_omap_deleted',
  'bitlocker_detect',
  'bitlocker_fve',
  'luks_detect',
  'spaces_detect',
  'vss_unbound',
  'vss_bind',
  'vss_snapshot',
  'hfs_limit',
  'hfs_vh',
  'hfs_vol_name',
  'hfs_catalog_unread',
  'hfs_linear_unread',
  'apfs_nxsb_unread',
  'apfs_block_unread',
  'apfs_linear_unread',
  'apfs_catalog_unread',
  'usn_journal',
  'usn_unread',
  'ntfs_logfile',
  'ntfs_logfile_restart',
  'ntfs_logfile_unread',
  'ntfs_recycle_meta',
  'ntfs_i30',
  'ntfs_i30_unalloc',
  'ntfs_i30_unread',
  'ntfs_mft_unread',
  'ntfs_efs',
  'ntfs_object_id',
  'ntfs_vol_name',
  'Folder',
  'refs_volume',
  'refs_supb_unread',
  'refs_page_unread',
  'fat_dir_unread',
  'fat_chain_unread',
  'fat_vol_label',
  'iso9660_vol_id',
  'exfat_vol_label',
  'udf_vol_id',
  'udf_pvd_id',
  'udf_fsd_id',
  'ext4_dir_unread',
  'ext4_journal',
  'ext4_journal_unread',
  'ext4_vol_name',
  'xfs_dir_unread',
  'xfs_sb_unread',
  'xfs_inode_unread',
  'xfs_bmap_unread',
  'xfs_vol_name',
  'unalloc_map_unread',
  'probe_unread',
  'carver_unread',
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
  if (source === 'apfs_extent' || source === 'hfs_catalog' || source === 'hfs_catalog_unused' || source === 'hfs_journal' || source === 'xfs_unlinked' || source === 'ext4_journal_replay') return !!hasRuns
  if (source === 'ntfs_thumbcache') return true
  return true
}

/** i18n key for sources with a curated label; callers translate via t().
 *  Unknown sources return the raw id (locale-neutral technical name). */
const KEY_BY_SOURCE: Record<string, string> = {
  apfs_container: 'source.apfs_container',
  apfs_volume: 'source.apfs_container',
  apfs_file: 'source.apfs_file',
  apfs_omap_deleted: 'source.apfs_omap_deleted',
  apfs_extent: 'source.apfs_extent',
  hfs_limit: 'source.hfs_limit',
  hfs_vol_name: 'source.hfs_vol_name',
  hfs_catalog_unread: 'source.hfs_catalog_unread',
  hfs_linear_unread: 'source.hfs_linear_unread',
  apfs_nxsb_unread: 'source.apfs_nxsb_unread',
  apfs_block_unread: 'source.apfs_block_unread',
  apfs_linear_unread: 'source.apfs_linear_unread',
  apfs_catalog_unread: 'source.apfs_catalog_unread',
  refs_volume: 'source.refs_volume',
  refs_supb_unread: 'source.refs_supb_unread',
  refs_page_unread: 'source.refs_page_unread',
  fat_dir_unread: 'source.fat_dir_unread',
  fat_chain_unread: 'source.fat_chain_unread',
  fat_vol_label: 'source.fat_vol_label',
  iso9660_vol_id: 'source.iso9660_vol_id',
  exfat_vol_label: 'source.exfat_vol_label',
  udf_vol_id: 'source.udf_vol_id',
  udf_pvd_id: 'source.udf_pvd_id',
  udf_fsd_id: 'source.udf_fsd_id',
  ext4_dir_unread: 'source.ext4_dir_unread',
  ext4_journal: 'source.ext4_journal',
  ext4_journal_unread: 'source.ext4_journal_unread',
  ext4_journal_replay: 'source.ext4_journal_replay',
  ext4_vol_name: 'source.ext4_vol_name',
  xfs_dir_unread: 'source.xfs_dir_unread',
  xfs_sb_unread: 'source.xfs_sb_unread',
  xfs_inode_unread: 'source.xfs_inode_unread',
  xfs_bmap_unread: 'source.xfs_bmap_unread',
  xfs_vol_name: 'source.xfs_vol_name',
  unalloc_map_unread: 'source.unalloc_map_unread',
  ntfs_i30_unread: 'source.ntfs_i30_unread',
  ntfs_logfile_unread: 'source.ntfs_logfile_unread',
  usn_unread: 'source.usn_unread',
  ntfs_mft_unread: 'source.ntfs_mft_unread',
  ntfs_efs: 'source.ntfs_efs',
  ntfs_object_id: 'source.ntfs_object_id',
  ntfs_vol_name: 'source.ntfs_vol_name',
  probe_unread: 'source.probe_unread',
  carver_unread: 'source.carver_unread',
  hfs_catalog: 'source.hfs_catalog',
  hfs_catalog_unused: 'source.hfs_catalog_unused',
  hfs_journal: 'source.hfs_journal',
  xfs_unlinked: 'source.xfs_unlinked',
  vss_unbound: 'source.vss_unbound',
  vss_bind: 'source.vss_bind',
  vss_snapshot: 'source.vss_snapshot',
  vss_ntfs: 'source.vss_ntfs',
  vss_fat: 'source.vss_fat',
  bitlocker_detect: 'source.bitlocker_detect',
  bitlocker_fve: 'source.bitlocker_fve',
  luks_detect: 'source.luks_detect',
  spaces_detect: 'source.spaces_detect',
  ntfs_mft_logfile: 'source.ntfs_mft_logfile',
  ntfs_mft_usn: 'source.ntfs_mft_usn',
  ntfs_recycle: 'source.ntfs_recycle',
  ntfs_recycle_meta: 'source.ntfs_recycle_meta',
  ntfs_i30: 'source.ntfs_i30',
  ntfs_i30_unalloc: 'source.ntfs_i30_unalloc',
  ntfs_i30_carve: 'source.ntfs_i30_carve',
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
