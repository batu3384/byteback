import { describe, expect, it, vi } from 'vitest'
import {
  isHfsLimitRecord,
  isHfsCatalogUnreadRecord,
  isApfsNxsbUnreadRecord,
  isRefsProbeCappedRecord,
  isRefsSupbUnreadRecord,
  isFatDirUnreadRecord,
  isExt4DirUnreadRecord,
  isXfsDirUnreadRecord,
  isNtfsI30UnreadRecord,
  isUnallocMapUnreadRecord,
  isNtfsLogfileUnreadRecord,
  isUsnUnreadRecord,
  isNtfsMftUnreadRecord,
  isProbeUnreadRecord,
  isCarverUnreadRecord,
  loadScanHonestyFlags,
  SCAN_HONESTY_HFS_FILTER,
  SCAN_HONESTY_HFS_CATALOG_FILTER,
  SCAN_HONESTY_APFS_FILTER,
  SCAN_HONESTY_REFS_FILTER,
  SCAN_HONESTY_REFS_SUPB_FILTER,
  SCAN_HONESTY_FAT_FILTER,
  SCAN_HONESTY_EXT4_FILTER,
  SCAN_HONESTY_XFS_FILTER,
  SCAN_HONESTY_I30_FILTER,
  SCAN_HONESTY_UNALLOC_FILTER,
  SCAN_HONESTY_LOGFILE_FILTER,
  SCAN_HONESTY_USN_FILTER,
  SCAN_HONESTY_MFT_FILTER,
  SCAN_HONESTY_PROBE_FILTER,
  SCAN_HONESTY_CARVE_FILTER,
  SCAN_HONESTY_LANES,
} from './scan-honesty'
import type { FileRecord } from './ipc-contract'

function rec(partial: Partial<FileRecord>): FileRecord {
  return {
    id: 1,
    name: 'x',
    sizeBytes: 0,
    status: 0,
    ...partial,
  }
}

const ALL_TRUE: Record<string, boolean> = {
  hfsLimit: true,
  hfsCatalogUnread: true,
  apfsNxsbUnread: true,
  refsProbeCapped: true,
  refsSupbUnread: true,
  fatDirUnread: true,
  ext4DirUnread: true,
  xfsDirUnread: true,
  ntfsI30Unread: true,
  unallocMapUnread: true,
  ntfsLogfileUnread: true,
  usnUnread: true,
  ntfsMftUnread: true,
  probeUnread: true,
  carverUnread: true,
}

describe('scan honesty sentinels', () => {
  it('recognizes HFS/ReFS caps and unread FAT/ext4/XFS/$I30 directories', () => {
    expect(isHfsLimitRecord({ source: 'hfs_limit' })).toBe(true)
    expect(isHfsLimitRecord({ source: 'hfs_catalog' })).toBe(false)
    expect(isHfsCatalogUnreadRecord({ source: 'hfs_catalog_unread' })).toBe(true)
    expect(isHfsCatalogUnreadRecord({ source: 'hfs_linear_unread' })).toBe(true)
    expect(isHfsCatalogUnreadRecord({ source: 'hfs_catalog' })).toBe(false)
    expect(isHfsCatalogUnreadRecord({ source: 'hfs_limit' })).toBe(false)
    expect(isApfsNxsbUnreadRecord({ source: 'apfs_nxsb_unread' })).toBe(true)
    expect(isApfsNxsbUnreadRecord({ source: 'apfs_block_unread' })).toBe(true)
    expect(isApfsNxsbUnreadRecord({ source: 'apfs_linear_unread' })).toBe(true)
    expect(isApfsNxsbUnreadRecord({ source: 'apfs_container' })).toBe(false)
    expect(isRefsProbeCappedRecord({ source: 'refs_volume', path: '/refs-probe-capped/' })).toBe(true)
    expect(isRefsProbeCappedRecord({ source: 'refs_volume', path: '/refs/' })).toBe(false)
    expect(isRefsSupbUnreadRecord({ source: 'refs_supb_unread' })).toBe(true)
    expect(isRefsSupbUnreadRecord({ source: 'refs_page_unread' })).toBe(true)
    expect(isRefsSupbUnreadRecord({ source: 'refs_volume' })).toBe(false)
    expect(isFatDirUnreadRecord({ source: 'fat_dir_unread' })).toBe(true)
    expect(isFatDirUnreadRecord({ source: 'fat_chain_unread' })).toBe(true)
    expect(isFatDirUnreadRecord({ source: 'fat' })).toBe(false)
    expect(isExt4DirUnreadRecord({ source: 'ext4_dir_unread' })).toBe(true)
    expect(isExt4DirUnreadRecord({ source: 'ext4_dirent' })).toBe(false)
    expect(isXfsDirUnreadRecord({ source: 'xfs_dir_unread' })).toBe(true)
    expect(isXfsDirUnreadRecord({ source: 'xfs_sb_unread' })).toBe(true)
    expect(isXfsDirUnreadRecord({ source: 'xfs_inode_unread' })).toBe(true)
    expect(isXfsDirUnreadRecord({ source: 'xfs_bmap_unread' })).toBe(true)
    expect(isXfsDirUnreadRecord({ source: 'xfs_inode' })).toBe(false)
    expect(isNtfsI30UnreadRecord({ source: 'ntfs_i30_unread' })).toBe(true)
    expect(isNtfsI30UnreadRecord({ source: 'ntfs_i30' })).toBe(false)
    expect(isUnallocMapUnreadRecord({ source: 'unalloc_map_unread' })).toBe(true)
    expect(isUnallocMapUnreadRecord({ source: 'carver' })).toBe(false)
    expect(isNtfsLogfileUnreadRecord({ source: 'ntfs_logfile_unread' })).toBe(true)
    expect(isNtfsLogfileUnreadRecord({ source: 'ntfs_logfile' })).toBe(false)
    expect(isUsnUnreadRecord({ source: 'usn_unread' })).toBe(true)
    expect(isUsnUnreadRecord({ source: 'usn_journal' })).toBe(false)
    expect(isNtfsMftUnreadRecord({ source: 'ntfs_mft_unread' })).toBe(true)
    expect(isNtfsMftUnreadRecord({ source: 'ntfs_mft' })).toBe(false)
    expect(isProbeUnreadRecord({ source: 'probe_unread' })).toBe(true)
    expect(isProbeUnreadRecord({ source: 'ntfs_mft' })).toBe(false)
    expect(isCarverUnreadRecord({ source: 'carver_unread' })).toBe(true)
    expect(isCarverUnreadRecord({ source: 'carver' })).toBe(false)
  })

  it('pages discovery rows instead of relying on keyword search', async () => {
    const getFilesPage = vi.fn(async (_id: number, _off: number, _lim: number, filter?: { sourceLike?: string }) => {
      if (filter?.sourceLike === 'hfs_limit') return [rec({ source: 'hfs_limit', name: 'catalog truncated' })]
      if (filter?.sourceLike === 'hfs_%_unread') {
        return [rec({ source: 'hfs_linear_unread', name: 'Hfs_LinearUnread', path: '/hfs-linear-unread/' })]
      }
      if (filter?.sourceLike === 'apfs_%_unread') {
        return [rec({ source: 'apfs_nxsb_unread', name: 'Apfs_NxsbUnread', path: '/apfs-nxsb-unread/' })]
      }
      if (filter?.sourceLike === 'refs_volume') {
        return [rec({ source: 'refs_volume', name: 'ReFS_Volume', path: '/refs-probe-capped/' })]
      }
      if (filter?.sourceLike === 'refs_%_unread') {
        return [rec({ source: 'refs_supb_unread', name: 'Refs_SupbUnread', path: '/refs-supb-unread/' })]
      }
      if (filter?.sourceLike === 'fat_%_unread') {
        return [rec({ source: 'fat_dir_unread', name: 'FAT_DirectoryUnread', path: '/fat-dir-unread/' })]
      }
      if (filter?.sourceLike === 'ext4_dir_unread') {
        return [rec({ source: 'ext4_dir_unread', name: 'Ext4_DirectoryUnread', path: '/ext4-dir-unread/' })]
      }
      if (filter?.sourceLike === 'xfs_%_unread') {
        return [rec({ source: 'xfs_dir_unread', name: 'Xfs_DirectoryUnread', path: '/xfs-dir-unread/' })]
      }
      if (filter?.sourceLike === 'ntfs_i30_unread') {
        return [rec({ source: 'ntfs_i30_unread', name: 'Ntfs_I30Unread', path: '/ntfs-i30-unread/' })]
      }
      if (filter?.sourceLike === 'unalloc_map_unread') {
        return [rec({ source: 'unalloc_map_unread', name: 'Unalloc_MapUnread', path: '/unalloc-map-unread/' })]
      }
      if (filter?.sourceLike === 'ntfs_logfile_unread') {
        return [rec({ source: 'ntfs_logfile_unread', name: 'Ntfs_LogfileUnread', path: '/ntfs-logfile-unread/' })]
      }
      if (filter?.sourceLike === 'usn_unread') {
        return [rec({ source: 'usn_unread', name: 'Ntfs_UsnUnread', path: '/usn-unread/' })]
      }
      if (filter?.sourceLike === 'ntfs_mft_unread') {
        return [rec({ source: 'ntfs_mft_unread', name: 'Ntfs_MftUnread', path: '/ntfs-mft-unread/' })]
      }
      if (filter?.sourceLike === 'probe_unread') {
        return [rec({ source: 'probe_unread', name: 'Probe_Unread', path: '/probe-unread/' })]
      }
      if (filter?.sourceLike === 'carver_unread') {
        return [rec({ source: 'carver_unread', name: 'Carver_Unread', path: '/carver-unread/' })]
      }
      return []
    })
    const flags = await loadScanHonestyFlags(9, getFilesPage, [])
    expect(flags).toEqual(ALL_TRUE)
    expect(getFilesPage).toHaveBeenCalledWith(9, 0, 1, SCAN_HONESTY_HFS_FILTER)
    expect(getFilesPage).toHaveBeenCalledWith(9, 0, 1, SCAN_HONESTY_HFS_CATALOG_FILTER)
    expect(getFilesPage).toHaveBeenCalledWith(9, 0, 1, SCAN_HONESTY_APFS_FILTER)
    expect(getFilesPage).toHaveBeenCalledWith(9, 0, 8, SCAN_HONESTY_REFS_FILTER)
    expect(getFilesPage).toHaveBeenCalledWith(9, 0, 1, SCAN_HONESTY_REFS_SUPB_FILTER)
    expect(getFilesPage).toHaveBeenCalledWith(9, 0, 1, SCAN_HONESTY_FAT_FILTER)
    expect(getFilesPage).toHaveBeenCalledWith(9, 0, 1, SCAN_HONESTY_EXT4_FILTER)
    expect(getFilesPage).toHaveBeenCalledWith(9, 0, 1, SCAN_HONESTY_XFS_FILTER)
    expect(getFilesPage).toHaveBeenCalledWith(9, 0, 1, SCAN_HONESTY_I30_FILTER)
    expect(getFilesPage).toHaveBeenCalledWith(9, 0, 1, SCAN_HONESTY_UNALLOC_FILTER)
    expect(getFilesPage).toHaveBeenCalledWith(9, 0, 1, SCAN_HONESTY_LOGFILE_FILTER)
    expect(getFilesPage).toHaveBeenCalledWith(9, 0, 1, SCAN_HONESTY_USN_FILTER)
    expect(getFilesPage).toHaveBeenCalledWith(9, 0, 1, SCAN_HONESTY_MFT_FILTER)
    expect(getFilesPage).toHaveBeenCalledWith(9, 0, 1, SCAN_HONESTY_PROBE_FILTER)
    expect(getFilesPage).toHaveBeenCalledWith(9, 0, 1, SCAN_HONESTY_CARVE_FILTER)
  })

  it('keeps one lane table as fetch and banner source', async () => {
    expect(SCAN_HONESTY_LANES).toHaveLength(15)
    expect(SCAN_HONESTY_LANES.map((l) => l.filter)).toEqual([
      SCAN_HONESTY_HFS_FILTER,
      SCAN_HONESTY_HFS_CATALOG_FILTER,
      SCAN_HONESTY_APFS_FILTER,
      SCAN_HONESTY_REFS_FILTER,
      SCAN_HONESTY_REFS_SUPB_FILTER,
      SCAN_HONESTY_FAT_FILTER,
      SCAN_HONESTY_EXT4_FILTER,
      SCAN_HONESTY_XFS_FILTER,
      SCAN_HONESTY_I30_FILTER,
      SCAN_HONESTY_UNALLOC_FILTER,
      SCAN_HONESTY_LOGFILE_FILTER,
      SCAN_HONESTY_USN_FILTER,
      SCAN_HONESTY_MFT_FILTER,
      SCAN_HONESTY_PROBE_FILTER,
      SCAN_HONESTY_CARVE_FILTER,
    ])
    const getFilesPage = vi.fn(async () => [] as FileRecord[])
    await loadScanHonestyFlags(4, getFilesPage, [])
    expect(getFilesPage).toHaveBeenCalledTimes(SCAN_HONESTY_LANES.length)
    SCAN_HONESTY_LANES.forEach((lane) => {
      expect(getFilesPage).toHaveBeenCalledWith(4, 0, lane.pageLimit, lane.filter)
    })
  })

  it('does not query when local rows already carry all sentinels', async () => {
    const getFilesPage = vi.fn()
    const flags = await loadScanHonestyFlags(3, getFilesPage, [
      { source: 'hfs_limit' },
      { source: 'hfs_catalog_unread' },
      { source: 'apfs_nxsb_unread' },
      { source: 'refs_volume', path: '/refs-probe-capped/' },
      { source: 'refs_supb_unread' },
      { source: 'fat_chain_unread' },
      { source: 'ext4_dir_unread' },
      { source: 'xfs_dir_unread' },
      { source: 'ntfs_i30_unread' },
      { source: 'unalloc_map_unread' },
      { source: 'ntfs_logfile_unread' },
      { source: 'usn_unread' },
      { source: 'ntfs_mft_unread' },
      { source: 'probe_unread' },
      { source: 'carver_unread' },
    ])
    expect(flags).toEqual(ALL_TRUE)
    expect(getFilesPage).not.toHaveBeenCalled()
  })
})
