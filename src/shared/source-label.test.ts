import { describe, it, expect } from 'vitest'
import { sourceLabelKey, localizeSourceLabel, isDiscoveryOnlySource, canRecoverSource, isRecoverableListSource, isDuplicateSource } from './source-label'

describe('sourceLabelKey', () => {
  it('maps APFS container and volume hits to curated keys', () => {
    expect(sourceLabelKey('apfs_container')).toBe('source.apfs_container')
    expect(sourceLabelKey('apfs_volume')).toBe('source.apfs_container')
    expect(sourceLabelKey('apfs_file')).toBe('source.apfs_file')
  })

  it('marks HFS catalog ceiling records', () => {
    expect(sourceLabelKey('hfs_limit')).toBe('source.hfs_limit')
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
      'apfs_container', 'apfs_volume', 'apfs_file',
      'bitlocker_detect', 'bitlocker_fve',
      'vss_unbound', 'vss_bind', 'vss_snapshot',
      'hfs_limit', 'hfs_vh', 'hfs_catalog',
      'usn_journal', 'ntfs_logfile', 'ntfs_logfile_restart', 'ntfs_recycle_meta',
      'ntfs_i30', 'Folder', 'refs_volume',
    ]
    for (const s of expected) expect(isDiscoveryOnlySource(s)).toBe(true)
    expect(isDiscoveryOnlySource('carver_duplicate')).toBe(false) // duplicate gate separate in TS
    expect(isDuplicateSource('carver_duplicate')).toBe(true)
  })
})
