import { describe, it, expect } from 'vitest'
import { sourceLabelKey, localizeSourceLabel, isDiscoveryOnlySource, canRecoverSource, isRecoverableListSource, isDuplicateSource } from './source-label'

describe('sourceLabelKey', () => {
  it('maps APFS container and volume hits to curated keys', () => {
    expect(sourceLabelKey('apfs_container')).toBe('source.apfs_container')
    expect(sourceLabelKey('apfs_volume')).toBe('source.apfs_container')
    expect(sourceLabelKey('apfs_file')).toBe('source.apfs_file')
    expect(sourceLabelKey('apfs_omap_deleted')).toBe('source.apfs_omap_deleted')
  })

  it('marks HFS catalog ceiling records', () => {
    expect(sourceLabelKey('hfs_limit')).toBe('source.hfs_limit')
    expect(sourceLabelKey('hfs_catalog')).toBe('source.hfs_catalog')
    expect(sourceLabelKey('hfs_catalog_unused')).toBe('source.hfs_catalog_unused')
    expect(sourceLabelKey('hfs_journal')).toBe('source.hfs_journal')
    expect(sourceLabelKey('xfs_unlinked')).toBe('source.xfs_unlinked')
    expect(sourceLabelKey('hfs_catalog_unread')).toBe('source.hfs_catalog_unread')
    expect(sourceLabelKey('hfs_linear_unread')).toBe('source.hfs_linear_unread')
    expect(sourceLabelKey('apfs_nxsb_unread')).toBe('source.apfs_nxsb_unread')
    expect(sourceLabelKey('apfs_block_unread')).toBe('source.apfs_block_unread')
    expect(sourceLabelKey('apfs_linear_unread')).toBe('source.apfs_linear_unread')
    expect(sourceLabelKey('apfs_catalog_unread')).toBe('source.apfs_catalog_unread')
    expect(sourceLabelKey('refs_supb_unread')).toBe('source.refs_supb_unread')
    expect(sourceLabelKey('refs_page_unread')).toBe('source.refs_page_unread')
    expect(sourceLabelKey('refs_volume')).toBe('source.refs_volume')
    expect(sourceLabelKey('fat_dir_unread')).toBe('source.fat_dir_unread')
    expect(sourceLabelKey('fat_chain_unread')).toBe('source.fat_chain_unread')
    expect(sourceLabelKey('fat_vol_label')).toBe('source.fat_vol_label')
    expect(sourceLabelKey('iso9660_vol_id')).toBe('source.iso9660_vol_id')
    expect(sourceLabelKey('exfat_vol_label')).toBe('source.exfat_vol_label')
    expect(sourceLabelKey('udf_vol_id')).toBe('source.udf_vol_id')
    expect(sourceLabelKey('udf_pvd_id')).toBe('source.udf_pvd_id')
    expect(sourceLabelKey('udf_fsd_id')).toBe('source.udf_fsd_id')
    expect(sourceLabelKey('ext4_dir_unread')).toBe('source.ext4_dir_unread')
    expect(sourceLabelKey('ext4_journal')).toBe('source.ext4_journal')
    expect(sourceLabelKey('ext4_journal_unread')).toBe('source.ext4_journal_unread')
    expect(sourceLabelKey('ext4_journal_replay')).toBe('source.ext4_journal_replay')
    expect(sourceLabelKey('xfs_dir_unread')).toBe('source.xfs_dir_unread')
    expect(sourceLabelKey('xfs_sb_unread')).toBe('source.xfs_sb_unread')
    expect(sourceLabelKey('xfs_inode_unread')).toBe('source.xfs_inode_unread')
    expect(sourceLabelKey('xfs_bmap_unread')).toBe('source.xfs_bmap_unread')
    expect(sourceLabelKey('unalloc_map_unread')).toBe('source.unalloc_map_unread')
    expect(sourceLabelKey('ntfs_i30_unread')).toBe('source.ntfs_i30_unread')
    expect(sourceLabelKey('ntfs_logfile_unread')).toBe('source.ntfs_logfile_unread')
    expect(sourceLabelKey('usn_unread')).toBe('source.usn_unread')
    expect(sourceLabelKey('ntfs_mft_unread')).toBe('source.ntfs_mft_unread')
    expect(sourceLabelKey('probe_unread')).toBe('source.probe_unread')
    expect(sourceLabelKey('carver_unread')).toBe('source.carver_unread')
  })

  it('maps unbound VSS and BitLocker discovery', () => {
    expect(sourceLabelKey('vss_unbound')).toBe('source.vss_unbound')
    expect(sourceLabelKey('vss_bind')).toBe('source.vss_bind')
    expect(sourceLabelKey('bitlocker_detect')).toBe('source.bitlocker_detect')
    expect(sourceLabelKey('bitlocker_fve')).toBe('source.bitlocker_fve')
  })

  it('passes through other sources', () => {
    expect(sourceLabelKey('mft')).toBe('mft')
    expect(sourceLabelKey(undefined)).toBe('')
    expect(localizeSourceLabel('mft', (k) => k)).toBe('mft')
    expect(localizeSourceLabel('apfs_file', (k) => `T:${k}`)).toBe('T:source.apfs_file')
  })
})

describe('isDiscoveryOnlySource', () => {
  it('blocks recover on metadata records', () => {
    expect(isDiscoveryOnlySource('apfs_file')).toBe(true)
    expect(isDiscoveryOnlySource('vss_ntfs')).toBe(false)
    expect(canRecoverSource('apfs_extent', true)).toBe(true)
    expect(canRecoverSource('apfs_extent', false)).toBe(false)
    expect(canRecoverSource('ntfs_mft')).toBe(true)
    expect(canRecoverSource('ntfs_mft_logfile')).toBe(true)
  })

  it('excludes timeline and logfile hints from recoverable list', () => {
    expect(isRecoverableListSource('usn_journal')).toBe(false)
    expect(isRecoverableListSource('ntfs_logfile')).toBe(false)
    expect(isRecoverableListSource('ntfs_mft')).toBe(true)
  })

  it('labels logfile-verified MFT', () => {
    expect(sourceLabelKey('ntfs_mft_logfile')).toBe('source.ntfs_mft_logfile')
    expect(sourceLabelKey('ntfs_mft_usn')).toBe('source.ntfs_mft_usn')
    expect(sourceLabelKey('ntfs_recycle')).toBe('source.ntfs_recycle')
    expect(isDiscoveryOnlySource('ntfs_recycle_meta')).toBe(true)
    expect(canRecoverSource('ntfs_i30', false)).toBe(false)
    expect(canRecoverSource('ntfs_i30', true)).toBe(false)
    expect(isDiscoveryOnlySource('ntfs_i30_unalloc')).toBe(true)
    expect(canRecoverSource('ntfs_i30_unalloc', true)).toBe(false)
    expect(isDiscoveryOnlySource('ntfs_i30_carve')).toBe(false)
    expect(canRecoverSource('ntfs_i30_carve', true)).toBe(true)
    expect(sourceLabelKey('ntfs_i30_carve')).toBe('source.ntfs_i30_carve')
    expect(isDiscoveryOnlySource('ntfs_i30')).toBe(true)
    expect(canRecoverSource('ntfs_thumbcache', false)).toBe(true)
    expect(sourceLabelKey('ntfs_thumbcache')).toBe('source.ntfs_thumbcache')
  })

  it('hides carve duplicates from default recoverable list', () => {
    expect(isDuplicateSource('carver_duplicate')).toBe(true)
    expect(isRecoverableListSource('carver_duplicate')).toBe(false)
    expect(isRecoverableListSource('carver')).toBe(true)
  })

  it('lists the same discovery sources native expects', () => {
    // Mirrors native/include/scan/discovery_sources.h — fail loudly on drift.
    const expected = [
      'apfs_container', 'apfs_volume', 'apfs_file', 'apfs_omap_deleted',
      'bitlocker_detect', 'bitlocker_fve', 'luks_detect', 'spaces_detect',
      'vss_unbound', 'vss_bind', 'vss_snapshot',
      'hfs_limit', 'hfs_vh', 'hfs_vol_name', 'hfs_catalog_unread', 'hfs_linear_unread', 'apfs_nxsb_unread', 'apfs_block_unread', 'apfs_linear_unread', 'apfs_catalog_unread',
      'usn_journal', 'usn_unread', 'ntfs_logfile', 'ntfs_logfile_restart', 'ntfs_logfile_unread', 'ntfs_recycle_meta',
      'ntfs_i30', 'ntfs_i30_unalloc', 'ntfs_i30_unread', 'ntfs_mft_unread', 'ntfs_efs', 'ntfs_object_id', 'ntfs_vol_name', 'Folder', 'refs_volume', 'refs_supb_unread', 'refs_page_unread',
      'fat_dir_unread', 'fat_chain_unread', 'fat_vol_label', 'iso9660_vol_id', 'exfat_vol_label', 'udf_vol_id', 'udf_pvd_id', 'udf_fsd_id', 'ext4_dir_unread', 'ext4_journal', 'ext4_journal_unread', 'ext4_vol_name', 'xfs_dir_unread', 'xfs_sb_unread', 'xfs_inode_unread', 'xfs_bmap_unread', 'xfs_vol_name',
      'unalloc_map_unread', 'probe_unread', 'carver_unread',
    ]
    for (const s of expected) expect(isDiscoveryOnlySource(s)).toBe(true)
    expect(isDiscoveryOnlySource('hfs_catalog')).toBe(false)
    expect(isDiscoveryOnlySource('hfs_catalog_unused')).toBe(false)
    expect(canRecoverSource('hfs_catalog', true)).toBe(true)
    expect(canRecoverSource('hfs_catalog', false)).toBe(false)
    expect(canRecoverSource('hfs_catalog_unused', true)).toBe(true)
    expect(canRecoverSource('hfs_catalog_unused', false)).toBe(false)
    expect(isDiscoveryOnlySource('hfs_journal')).toBe(false)
    expect(canRecoverSource('hfs_journal', true)).toBe(true)
    expect(canRecoverSource('hfs_journal', false)).toBe(false)
    expect(isDiscoveryOnlySource('xfs_unlinked')).toBe(false)
    expect(canRecoverSource('xfs_unlinked', true)).toBe(true)
    expect(canRecoverSource('xfs_unlinked', false)).toBe(false)
    expect(canRecoverSource('ext4_journal_replay', true)).toBe(true)
    expect(canRecoverSource('ext4_journal_replay', false)).toBe(false)
    expect(isDuplicateSource('carver_duplicate')).toBe(true)
  })
})
