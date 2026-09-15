import type { FileListFilter, FileRecord } from './ipc-contract'

/** Discovery rows that must surface as examiner warnings, not hide in filters. */
export const SCAN_HONESTY_HFS_FILTER: FileListFilter = {
  includeDiscovery: true,
  sourceLike: 'hfs_limit',
}

export const SCAN_HONESTY_HFS_CATALOG_FILTER: FileListFilter = {
  includeDiscovery: true,
  sourceLike: 'hfs_%_unread',
}

export const SCAN_HONESTY_APFS_FILTER: FileListFilter = {
  includeDiscovery: true,
  sourceLike: 'apfs_%_unread',
}

export const SCAN_HONESTY_REFS_FILTER: FileListFilter = {
  includeDiscovery: true,
  sourceLike: 'refs_volume',
}

export const SCAN_HONESTY_REFS_SUPB_FILTER: FileListFilter = {
  includeDiscovery: true,
  sourceLike: 'refs_%_unread',
}

export const SCAN_HONESTY_FAT_FILTER: FileListFilter = {
  includeDiscovery: true,
  sourceLike: 'fat_%_unread',
}

export const SCAN_HONESTY_EXT4_FILTER: FileListFilter = {
  includeDiscovery: true,
  sourceLike: 'ext4_dir_unread',
}

export const SCAN_HONESTY_XFS_FILTER: FileListFilter = {
  includeDiscovery: true,
  sourceLike: 'xfs_%_unread',
}

export const SCAN_HONESTY_I30_FILTER: FileListFilter = {
  includeDiscovery: true,
  sourceLike: 'ntfs_i30_unread',
}

export const SCAN_HONESTY_UNALLOC_FILTER: FileListFilter = {
  includeDiscovery: true,
  sourceLike: 'unalloc_map_unread',
}

export const SCAN_HONESTY_LOGFILE_FILTER: FileListFilter = {
  includeDiscovery: true,
  sourceLike: 'ntfs_logfile_unread',
}

export const SCAN_HONESTY_USN_FILTER: FileListFilter = {
  includeDiscovery: true,
  sourceLike: 'usn_unread',
}

export const SCAN_HONESTY_MFT_FILTER: FileListFilter = {
  includeDiscovery: true,
  sourceLike: 'ntfs_mft_unread',
}

export const SCAN_HONESTY_PROBE_FILTER: FileListFilter = {
  includeDiscovery: true,
  sourceLike: 'probe_unread',
}

export const SCAN_HONESTY_CARVE_FILTER: FileListFilter = {
  includeDiscovery: true,
  sourceLike: 'carver_unread',
}

export type ScanHonestyFlags = {
  hfsLimit: boolean
  hfsCatalogUnread: boolean
  apfsNxsbUnread: boolean
  refsProbeCapped: boolean
  refsSupbUnread: boolean
  fatDirUnread: boolean
  ext4DirUnread: boolean
  xfsDirUnread: boolean
  ntfsI30Unread: boolean
  unallocMapUnread: boolean
  ntfsLogfileUnread: boolean
  usnUnread: boolean
  ntfsMftUnread: boolean
  probeUnread: boolean
  carverUnread: boolean
}

export function isHfsLimitRecord(f: { source?: string }): boolean {
  return f.source === 'hfs_limit'
}

export function isHfsCatalogUnreadRecord(f: { source?: string }): boolean {
  return f.source === 'hfs_catalog_unread' || f.source === 'hfs_linear_unread'
}

export function isApfsNxsbUnreadRecord(f: { source?: string }): boolean {
  return f.source === 'apfs_nxsb_unread' || f.source === 'apfs_block_unread' ||
    f.source === 'apfs_linear_unread'
}

export function isRefsProbeCappedRecord(f: { source?: string; path?: string }): boolean {
  const path = f.path ?? ''
  return f.source === 'refs_volume' && path.includes('refs-probe-capped')
}

export function isRefsSupbUnreadRecord(f: { source?: string }): boolean {
  return f.source === 'refs_supb_unread' || f.source === 'refs_page_unread'
}

export function isFatDirUnreadRecord(f: { source?: string }): boolean {
  return f.source === 'fat_dir_unread' || f.source === 'fat_chain_unread'
}

export function isExt4DirUnreadRecord(f: { source?: string }): boolean {
  return f.source === 'ext4_dir_unread'
}

export function isXfsDirUnreadRecord(f: { source?: string }): boolean {
  return f.source === 'xfs_dir_unread' || f.source === 'xfs_sb_unread' ||
    f.source === 'xfs_inode_unread' || f.source === 'xfs_bmap_unread'
}

export function isNtfsI30UnreadRecord(f: { source?: string }): boolean {
  return f.source === 'ntfs_i30_unread'
}

export function isUnallocMapUnreadRecord(f: { source?: string }): boolean {
  return f.source === 'unalloc_map_unread'
}

export function isNtfsLogfileUnreadRecord(f: { source?: string }): boolean {
  return f.source === 'ntfs_logfile_unread'
}

export function isUsnUnreadRecord(f: { source?: string }): boolean {
  return f.source === 'usn_unread'
}

export function isNtfsMftUnreadRecord(f: { source?: string }): boolean {
  return f.source === 'ntfs_mft_unread'
}

export function isProbeUnreadRecord(f: { source?: string }): boolean {
  return f.source === 'probe_unread'
}

export function isCarverUnreadRecord(f: { source?: string }): boolean {
  return f.source === 'carver_unread'
}

type PageFn = (
  scanId: number,
  offset: number,
  limit: number,
  filter?: FileListFilter,
) => Promise<FileRecord[]>

function allHonestySet(flags: ScanHonestyFlags): boolean {
  return flags.hfsLimit && flags.hfsCatalogUnread && flags.apfsNxsbUnread && flags.refsProbeCapped &&
    flags.refsSupbUnread && flags.fatDirUnread &&
    flags.ext4DirUnread && flags.xfsDirUnread && flags.ntfsI30Unread &&
    flags.unallocMapUnread && flags.ntfsLogfileUnread && flags.usnUnread &&
    flags.ntfsMftUnread && flags.probeUnread && flags.carverUnread
}

/** Keyword search drops discovery rows; honesty banners must page them in. */
export async function loadScanHonestyFlags(
  scanId: number,
  getFilesPage: PageFn | undefined,
  localRows: Array<{ source?: string; path?: string }>,
): Promise<ScanHonestyFlags> {
  const flags: ScanHonestyFlags = {
    hfsLimit: localRows.some(isHfsLimitRecord),
    hfsCatalogUnread: localRows.some(isHfsCatalogUnreadRecord),
    apfsNxsbUnread: localRows.some(isApfsNxsbUnreadRecord),
    refsProbeCapped: localRows.some(isRefsProbeCappedRecord),
    refsSupbUnread: localRows.some(isRefsSupbUnreadRecord),
    fatDirUnread: localRows.some(isFatDirUnreadRecord),
    ext4DirUnread: localRows.some(isExt4DirUnreadRecord),
    xfsDirUnread: localRows.some(isXfsDirUnreadRecord),
    ntfsI30Unread: localRows.some(isNtfsI30UnreadRecord),
    unallocMapUnread: localRows.some(isUnallocMapUnreadRecord),
    ntfsLogfileUnread: localRows.some(isNtfsLogfileUnreadRecord),
    usnUnread: localRows.some(isUsnUnreadRecord),
    ntfsMftUnread: localRows.some(isNtfsMftUnreadRecord),
    probeUnread: localRows.some(isProbeUnreadRecord),
    carverUnread: localRows.some(isCarverUnreadRecord),
  }
  if (scanId <= 0 || !getFilesPage || allHonestySet(flags)) return flags
  const empty = Promise.resolve([] as FileRecord[])
  const [hfs, hfsCat, apfs, refs, refsSupb, fat, ext4, xfs, i30, unalloc, logfile, usn, mft, probe, carve] = await Promise.all([
    flags.hfsLimit ? empty : getFilesPage(scanId, 0, 1, SCAN_HONESTY_HFS_FILTER),
    flags.hfsCatalogUnread ? empty : getFilesPage(scanId, 0, 1, SCAN_HONESTY_HFS_CATALOG_FILTER),
    flags.apfsNxsbUnread ? empty : getFilesPage(scanId, 0, 1, SCAN_HONESTY_APFS_FILTER),
    flags.refsProbeCapped ? empty : getFilesPage(scanId, 0, 8, SCAN_HONESTY_REFS_FILTER),
    flags.refsSupbUnread ? empty : getFilesPage(scanId, 0, 1, SCAN_HONESTY_REFS_SUPB_FILTER),
    flags.fatDirUnread ? empty : getFilesPage(scanId, 0, 1, SCAN_HONESTY_FAT_FILTER),
    flags.ext4DirUnread ? empty : getFilesPage(scanId, 0, 1, SCAN_HONESTY_EXT4_FILTER),
    flags.xfsDirUnread ? empty : getFilesPage(scanId, 0, 1, SCAN_HONESTY_XFS_FILTER),
    flags.ntfsI30Unread ? empty : getFilesPage(scanId, 0, 1, SCAN_HONESTY_I30_FILTER),
    flags.unallocMapUnread ? empty : getFilesPage(scanId, 0, 1, SCAN_HONESTY_UNALLOC_FILTER),
    flags.ntfsLogfileUnread ? empty : getFilesPage(scanId, 0, 1, SCAN_HONESTY_LOGFILE_FILTER),
    flags.usnUnread ? empty : getFilesPage(scanId, 0, 1, SCAN_HONESTY_USN_FILTER),
    flags.ntfsMftUnread ? empty : getFilesPage(scanId, 0, 1, SCAN_HONESTY_MFT_FILTER),
    flags.probeUnread ? empty : getFilesPage(scanId, 0, 1, SCAN_HONESTY_PROBE_FILTER),
    flags.carverUnread ? empty : getFilesPage(scanId, 0, 1, SCAN_HONESTY_CARVE_FILTER),
  ])
  return {
    hfsLimit: flags.hfsLimit || hfs.some(isHfsLimitRecord),
    hfsCatalogUnread: flags.hfsCatalogUnread || hfsCat.some(isHfsCatalogUnreadRecord),
    apfsNxsbUnread: flags.apfsNxsbUnread || apfs.some(isApfsNxsbUnreadRecord),
    refsProbeCapped: flags.refsProbeCapped || refs.some(isRefsProbeCappedRecord),
    refsSupbUnread: flags.refsSupbUnread || refsSupb.some(isRefsSupbUnreadRecord),
    fatDirUnread: flags.fatDirUnread || fat.some(isFatDirUnreadRecord),
    ext4DirUnread: flags.ext4DirUnread || ext4.some(isExt4DirUnreadRecord),
    xfsDirUnread: flags.xfsDirUnread || xfs.some(isXfsDirUnreadRecord),
    ntfsI30Unread: flags.ntfsI30Unread || i30.some(isNtfsI30UnreadRecord),
    unallocMapUnread: flags.unallocMapUnread || unalloc.some(isUnallocMapUnreadRecord),
    ntfsLogfileUnread: flags.ntfsLogfileUnread || logfile.some(isNtfsLogfileUnreadRecord),
    usnUnread: flags.usnUnread || usn.some(isUsnUnreadRecord),
    ntfsMftUnread: flags.ntfsMftUnread || mft.some(isNtfsMftUnreadRecord),
    probeUnread: flags.probeUnread || probe.some(isProbeUnreadRecord),
    carverUnread: flags.carverUnread || carve.some(isCarverUnreadRecord),
  }
}
