import { describe, expect, it } from 'vitest'
import { canRecoverSource, isDiscoveryOnlySource, isRecoverableListSource } from './source-label'

describe('recover guards', () => {
  it('blocks discovery-only APFS catalog entries', () => {
    expect(isDiscoveryOnlySource('apfs_file')).toBe(true)
    expect(canRecoverSource('apfs_file', false)).toBe(false)
    expect(isRecoverableListSource('apfs_file')).toBe(false)
    expect(isDiscoveryOnlySource('apfs_omap_deleted')).toBe(true)
    expect(canRecoverSource('apfs_omap_deleted', true)).toBe(false)
  })

  it('allows HFS catalog when runs exist', () => {
    expect(canRecoverSource('hfs_catalog', true)).toBe(true)
    expect(canRecoverSource('hfs_catalog', false)).toBe(false)
    expect(isDiscoveryOnlySource('hfs_catalog')).toBe(false)
    expect(canRecoverSource('hfs_catalog_unused', true)).toBe(true)
    expect(canRecoverSource('hfs_catalog_unused', false)).toBe(false)
    expect(isDiscoveryOnlySource('hfs_catalog_unused')).toBe(false)
    expect(canRecoverSource('hfs_journal', true)).toBe(true)
    expect(canRecoverSource('hfs_journal', false)).toBe(false)
    expect(isDiscoveryOnlySource('hfs_journal')).toBe(false)
    expect(canRecoverSource('xfs_unlinked', true)).toBe(true)
    expect(canRecoverSource('xfs_unlinked', false)).toBe(false)
    expect(canRecoverSource('ext4_journal_replay', true)).toBe(true)
    expect(canRecoverSource('ext4_journal_replay', false)).toBe(false)
  })

  it('allows NTFS/MFT recoverable sources', () => {
    expect(canRecoverSource('ntfs_mft', true)).toBe(true)
    expect(isRecoverableListSource('ntfs_mft')).toBe(true)
  })
})
