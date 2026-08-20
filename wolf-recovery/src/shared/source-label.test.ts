import { describe, it, expect } from 'vitest'
import { sourceDisplayLabel } from './source-label'

describe('sourceDisplayLabel', () => {
  it('marks APFS container and volume hits as discovery-only', () => {
    expect(sourceDisplayLabel('apfs_container')).toBe('APFS volume (omap/fs_oid + APSB)')
    expect(sourceDisplayLabel('apfs_volume')).toBe('APFS volume (omap/fs_oid + APSB)')
    expect(sourceDisplayLabel('apfs_file')).toBe('APFS katalog')
  })

  it('marks HFS catalog ceiling records', () => {
    expect(sourceDisplayLabel('hfs_limit')).toBe('HFS katalog tavanı')
  })

  it('marks unbound VSS and BitLocker discovery', () => {
    expect(sourceDisplayLabel('vss_unbound')).toBe('VSS bağlanmadı (host karışımı kapalı)')
    expect(sourceDisplayLabel('vss_bind')).toBe('VSS (volume serial bağlandı)')
    expect(sourceDisplayLabel('bitlocker_detect')).toBe('BitLocker (FVE yok / anahtar yok)')
    expect(sourceDisplayLabel('bitlocker_fve')).toBe('BitLocker FVE metadata')
  })

  it('passes through other sources', () => {
    expect(sourceDisplayLabel('mft')).toBe('mft')
    expect(sourceDisplayLabel(undefined)).toBe('')
  })
})
