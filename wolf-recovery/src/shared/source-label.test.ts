import { describe, it, expect } from 'vitest'
import { sourceDisplayLabel, isDiscoveryOnlySource, canRecoverSource } from './source-label'

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
  })
})
