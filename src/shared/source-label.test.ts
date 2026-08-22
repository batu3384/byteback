import { describe, it, expect } from 'vitest'
import { sourceDisplayLabel, isDiscoveryOnlySource, canRecoverSource, isRecoverableListSource, isDuplicateSource } from './source-label'

describe('sourceDisplayLabel', () => {
  it('marks APFS container and volume hits as discovery-only', () => {
    expect(sourceDisplayLabel('apfs_container')).toBe('APFS keşif (nx_fs_oid + omap paddr + APSB)')
    expect(sourceDisplayLabel('apfs_volume')).toBe('APFS keşif (nx_fs_oid + omap paddr + APSB)')
    expect(sourceDisplayLabel('apfs_file')).toBe('APFS katalog adı (extent yok, kurtarılamaz)')
  })

  it('marks HFS catalog ceiling records', () => {
    expect(sourceDisplayLabel('hfs_limit')).toBe('HFS katalog tavanı (test/limit)')
  })

  it('marks unbound VSS and BitLocker discovery', () => {
    expect(sourceDisplayLabel('vss_unbound')).toBe('VSS bağlanmadı')
    expect(sourceDisplayLabel('vss_bind')).toBe('VSS bağlandı (serial+boyut)')
    expect(sourceDisplayLabel('bitlocker_detect')).toBe('BitLocker (anahtar yok, şifre kırma yok)')
    expect(sourceDisplayLabel('bitlocker_fve')).toBe('BitLocker FVE (kayıt decrypt değil)')
  })

  it('passes through other sources', () => {
    expect(sourceDisplayLabel('mft')).toBe('mft')
    expect(sourceDisplayLabel(undefined)).toBe('')
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
    expect(sourceDisplayLabel('ntfs_mft_logfile')).toBe('NTFS MFT (LogFile doğrulandı)')
    expect(sourceDisplayLabel('ntfs_mft_usn')).toBe('NTFS MFT (USN silme doğrulandı)')
    expect(sourceDisplayLabel('ntfs_recycle')).toContain('Geri Dönüşüm')
    expect(isDiscoveryOnlySource('ntfs_recycle_meta')).toBe(true)
    expect(canRecoverSource('ntfs_i30', false)).toBe(false)
    expect(canRecoverSource('ntfs_i30', true)).toBe(false)
    expect(isDiscoveryOnlySource('ntfs_i30')).toBe(true)
    expect(canRecoverSource('ntfs_thumbcache', false)).toBe(true)
    expect(sourceDisplayLabel('ntfs_thumbcache')).toContain('thumbcache')
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
