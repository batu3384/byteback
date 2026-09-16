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

export type ScanHonestyLane = {
  flag: keyof ScanHonestyFlags
  filter: FileListFilter
  pageLimit: number
  match: (f: { source?: string; path?: string }) => boolean
  testId: string
  messageKey: string
}

/** Single source for honesty fetch + banner testId/i18n. ScanView and ResultsView both consume this. */
export const SCAN_HONESTY_LANES: readonly ScanHonestyLane[] = [
  { flag: 'hfsLimit', filter: SCAN_HONESTY_HFS_FILTER, pageLimit: 1, match: isHfsLimitRecord, testId: 'hfs-limit-banner', messageKey: 'hfsLimit' },
  { flag: 'hfsCatalogUnread', filter: SCAN_HONESTY_HFS_CATALOG_FILTER, pageLimit: 1, match: isHfsCatalogUnreadRecord, testId: 'hfs-catalog-unread', messageKey: 'hfsCatalogUnread' },
  { flag: 'apfsNxsbUnread', filter: SCAN_HONESTY_APFS_FILTER, pageLimit: 1, match: isApfsNxsbUnreadRecord, testId: 'apfs-nxsb-unread', messageKey: 'apfsNxsbUnread' },
  { flag: 'refsProbeCapped', filter: SCAN_HONESTY_REFS_FILTER, pageLimit: 8, match: isRefsProbeCappedRecord, testId: 'refs-probe-capped', messageKey: 'refsProbeCapped' },
  { flag: 'refsSupbUnread', filter: SCAN_HONESTY_REFS_SUPB_FILTER, pageLimit: 1, match: isRefsSupbUnreadRecord, testId: 'refs-supb-unread', messageKey: 'refsSupbUnread' },
  { flag: 'fatDirUnread', filter: SCAN_HONESTY_FAT_FILTER, pageLimit: 1, match: isFatDirUnreadRecord, testId: 'fat-dir-unread', messageKey: 'fatDirUnread' },
  { flag: 'ext4DirUnread', filter: SCAN_HONESTY_EXT4_FILTER, pageLimit: 1, match: isExt4DirUnreadRecord, testId: 'ext4-dir-unread', messageKey: 'ext4DirUnread' },
  { flag: 'xfsDirUnread', filter: SCAN_HONESTY_XFS_FILTER, pageLimit: 1, match: isXfsDirUnreadRecord, testId: 'xfs-dir-unread', messageKey: 'xfsDirUnread' },
  { flag: 'ntfsI30Unread', filter: SCAN_HONESTY_I30_FILTER, pageLimit: 1, match: isNtfsI30UnreadRecord, testId: 'ntfs-i30-unread', messageKey: 'ntfsI30Unread' },
  { flag: 'unallocMapUnread', filter: SCAN_HONESTY_UNALLOC_FILTER, pageLimit: 1, match: isUnallocMapUnreadRecord, testId: 'unalloc-map-unread', messageKey: 'unallocMapUnread' },
  { flag: 'ntfsLogfileUnread', filter: SCAN_HONESTY_LOGFILE_FILTER, pageLimit: 1, match: isNtfsLogfileUnreadRecord, testId: 'ntfs-logfile-unread', messageKey: 'ntfsLogfileUnread' },
  { flag: 'usnUnread', filter: SCAN_HONESTY_USN_FILTER, pageLimit: 1, match: isUsnUnreadRecord, testId: 'usn-unread', messageKey: 'usnUnread' },
  { flag: 'ntfsMftUnread', filter: SCAN_HONESTY_MFT_FILTER, pageLimit: 1, match: isNtfsMftUnreadRecord, testId: 'ntfs-mft-unread', messageKey: 'ntfsMftUnread' },
  { flag: 'probeUnread', filter: SCAN_HONESTY_PROBE_FILTER, pageLimit: 1, match: isProbeUnreadRecord, testId: 'probe-unread', messageKey: 'probeUnread' },
  { flag: 'carverUnread', filter: SCAN_HONESTY_CARVE_FILTER, pageLimit: 1, match: isCarverUnreadRecord, testId: 'carver-unread', messageKey: 'carverUnread' },
]

export function emptyScanHonestyFlags(): ScanHonestyFlags {
  const flags = {} as ScanHonestyFlags
  for (const lane of SCAN_HONESTY_LANES) flags[lane.flag] = false
  return flags
}

type PageFn = (
  scanId: number,
  offset: number,
  limit: number,
  filter?: FileListFilter,
) => Promise<FileRecord[]>

function allHonestySet(flags: ScanHonestyFlags): boolean {
  return SCAN_HONESTY_LANES.every((lane) => flags[lane.flag])
}

/** Keyword search drops discovery rows; honesty banners must page them in. */
export async function loadScanHonestyFlags(
  scanId: number,
  getFilesPage: PageFn | undefined,
  localRows: Array<{ source?: string; path?: string }>,
): Promise<ScanHonestyFlags> {
  const flags = emptyScanHonestyFlags()
  for (const lane of SCAN_HONESTY_LANES) {
    flags[lane.flag] = localRows.some(lane.match)
  }
  if (scanId <= 0 || !getFilesPage || allHonestySet(flags)) return flags
  const pages = await Promise.all(
    SCAN_HONESTY_LANES.map((lane) =>
      flags[lane.flag]
        ? Promise.resolve([] as FileRecord[])
        : getFilesPage(scanId, 0, lane.pageLimit, lane.filter),
    ),
  )
  SCAN_HONESTY_LANES.forEach((lane, i) => {
    flags[lane.flag] = flags[lane.flag] || pages[i]!.some(lane.match)
  })
  return flags
}
