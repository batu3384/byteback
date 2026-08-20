import { describe, expect, it } from 'vitest'
import { canRecoverSource, isDiscoveryOnlySource, isRecoverableListSource } from './source-label'

describe('recover guards', () => {
  it('blocks discovery-only APFS catalog entries', () => {
    expect(isDiscoveryOnlySource('apfs_file')).toBe(true)
    expect(canRecoverSource('apfs_file', false)).toBe(false)
    expect(isRecoverableListSource('apfs_file')).toBe(false)
  })

  it('allows APFS extent when runs exist', () => {
    expect(canRecoverSource('apfs_extent', true)).toBe(true)
    expect(canRecoverSource('apfs_extent', false)).toBe(false)
  })

  it('allows NTFS/MFT recoverable sources', () => {
    expect(canRecoverSource('ntfs_mft', true)).toBe(true)
    expect(isRecoverableListSource('ntfs_mft')).toBe(true)
  })
})
