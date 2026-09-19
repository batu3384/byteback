/** Shared IPC contract between renderer, preload, and main process. */

export interface DriveInfo {
  index: number
  model: string
  serial: string
  sizeBytes: number
  sectorSize: number
  type: string
}

export interface DataRun {
  startSector: number
  sectorCount: number
  byteCount?: number
}

export interface FileRecord {
  id: number
  parentId?: number
  name: string
  extension?: string
  path?: string
  sizeBytes: number
  status: number
  confidence?: number
  category?: string
  source?: string
  startSector?: number
  endSector?: number
  compressed?: boolean
  createdAt?: number
  modifiedAt?: number
  runs?: DataRun[]
  contentHash?: string
  /** NTFS self MFT record number. -1/absent = unknown (not parentId). */
  mftRef?: number
  /** Transient: COUNT of files sharing (contentHash, sizeBytes) in this scan. >=2 = same-content group. */
  contentGroupSize?: number
  /** Content-search only: sanitized text around the first match (not persisted). */
  snippet?: string
  /** Byte offsets of the match span inside snippet; absent/-1 = no highlight. */
  snippetMatchStart?: number
  snippetMatchEnd?: number
}

export interface SmartStatus {
  isValid: boolean
  driveModel?: string
  healthScore?: string
  temperatureC?: number
  powerOnHours?: number
  reallocatedSectors?: number
  pendingSectors?: number
  isNvme?: boolean
  percentageUsed?: number
  availableSpare?: number
  availableSpareThreshold?: number
  criticalWarning?: number
  unsafeShutdowns?: number
  mediaErrors?: number
  totalBytesWritten?: number
  isSsd?: boolean
  seekPenaltyKnown?: boolean
}

export interface ScanState {
  id: number
  driveIndex: number
  scanType: string
  totalSectors: number
  scannedSectors: number
  status: number
  recoveredFiles?: number
  metadataComplete?: boolean
  carveResumeSector?: number
  startedAt?: number
  updatedAt?: number
  volumePath?: string
  evidenceDiskIndices?: number[]
}

export interface RecoverResult {
  success: boolean
  destPath?: string
  bytesRecovered?: number
  md5Hash?: string
  error?: string
  zeroFilled?: boolean
  validationScore?: number
  validationError?: string
  repairedPath?: string
}

export interface FilePreviewResult {
  success: boolean
  error?: string
  kind?: 'image' | 'text' | 'pdf' | 'binary' | string
  /** Sniffed MIME from native (e.g. image/jpeg). Empty/omitted when unknown. */
  mime?: string
  /** Structural hint when preview cannot render (e.g. H.264 IDR without decoder). */
  note?: string
  data?: Uint8Array | null
}

export interface ScanSummary {
  totalFiles: number
  deletedFiles: number
  imageFiles: number
  documentFiles: number
  videoFiles: number
  audioFiles: number
  archiveFiles: number
  carvedFiles?: number
  timelineEvents?: number
  usnCreates?: number
  usnDeletes?: number
  usnRenames?: number
}

export interface FileListFilter {
  status?: number
  category?: string
  query?: string
  sourceLike?: string
  /** Exclude sources matching LIKE (e.g. carver% from "deleted" metadata view). */
  sourceNotLike?: string
  includeDuplicates?: boolean
  includeDiscovery?: boolean
  /** CA-030: whitelisted sort key (e.g. "confidence_desc"); native maps it to SQL. */
  orderBy?: string
  /** P0-4: byte-size and unix-time range filters (native SQL, 0/undefined = off). */
  sizeMin?: number
  sizeMax?: number
  dateFrom?: number
  dateTo?: number
  /** FAZ 1.2 keyset cursor: sort-key value + id tiebreaker of the previous
   *  page's last row. Native ignores offset while a usable cursor applies;
   *  absent/null keeps OFFSET paging (first page, page jumps, CSV walk). */
  cursor?: { v: string | number; id: number } | null
}

export type ProgressCallback = (data: {
  current: number
  total: number
  badSectors?: number[]
  phase?: string
  /** P0-progress: phase-local counters (raw work units of the active phase). */
  phaseCurrent?: number
  phaseTotal?: number
}) => void
export type ScanCompleteCallback = (data: { scanId: number; status: number }) => void

export interface RaidAssemblyResult {
  success: boolean
  capacity: number
  numDisks: number
  error: string
}

export interface RaidState {
  active: boolean
  capacity: number
  numDisks: number
  level: number
  /** Indices of inactive member disks (native always emits, empty when healthy). */
  failedDisks?: number[]
  /** Physical drive indices bound into the virtual array (native always emits). */
  memberDriveIndices?: number[]
}

/** Parity-consistency autodetection over candidate member drives. */
export interface RaidDetection {
  found: boolean
  /** RaidLevel numbering as in reconstruct-raid (0=RAID0 … 4=RAID10, 5=JBOD, 6=RAID1E). Present when found. */
  raidLevel?: number
  /** Stripe/chunk size in bytes. Present when found. */
  blockSize?: number
  /** Per-member start offset in 512B sectors. Present when found. */
  dataOffsetSectors?: number
  /** Parity-consistency score 0..1. Present when found. */
  confidence?: number
  /** True when assembled MBR/NTFS/FAT/ext magic pinned the RAID5 stripe. */
  fsConfirmed?: boolean
  /** 0=left-asym, 1=left-sym (mdadm), 2=right-asym, 3=right-sym. Present when found. */
  raid5Algorithm?: number
  /** Slot permutation of the input member list. Empty/omitted = identity. */
  memberOrder?: number[]
  error?: string
}

export interface BatchRecoverResult {
  succeeded: number
  failed: number
  results: RecoverResult[]
  error?: string
}

export interface TimelineEvent {
  id: number
  timestamp: number
  eventType: string
  fileName: string
  mftRef: number
  source: string
}

export interface TimelineResult {
  total: number
  events: TimelineEvent[]
}

export interface PartitionInfo {
  type: string
  startSector: number
  sizeInSectors: number
  label: string
  isActive: boolean
}

/** TestDisk-style lost-partition hit. Size is neighbor-LBA estimate from the bridge. */
export interface LostPartitionHit {
  startSector: number
  sizeSectors: number
  fs: string
}

export interface LostPartitionScanResult {
  partitions: LostPartitionHit[]
  /** Step or EXT superblock I/O failed. Empty partitions is not “no volumes”. */
  unread: boolean
}

/** Optional partition scope for startScan (whole disk when omitted). */
export interface ScanOptions {
  partitionIndex?: number
  partitionStartSector?: number
  partitionSizeInSectors?: number
  resumeScanId?: number
  /** Required for deep/full_carve on SSD after TRIM warning. */
  allowSsdDeepScan?: boolean
  /** Windows volume device "\\.\X:" — scan binds this instead of PhysicalDrive+LBA. */
  volumePath?: string
  /** Local evidence image (RAW/E01/VHD family). driveIndex must be SCAN_IMAGE_DRIVE_INDEX (-2). */
  imagePath?: string
  /** PhysicalDrive indices of a spanned/striped volume (dest-on-source). */
  evidenceDiskIndices?: number[]
}

/** Logical drive letter resolved to PhysicalDrive + partition extent. */
export interface ResolvedVolume {
  driveIndex: number
  startSector: number
  sizeSectors: number
  fsType: string
  /** Windows disk-extent count. >1 = spanned/striped; scan uses volumePath. */
  diskExtentCount?: number
  volumePath?: string
  diskNumbers?: number[]
}

export interface CaseInfo {
  caseNumber: string
  investigator: string
  agency: string
  notes: string
  createdAt: number
  updatedAt: number
}

export interface NsrlStats {
  ok?: boolean
  count: number
  path: string
}

/** search-files IPC response — empty rows with error means native/DB failure, not zero hits. */
export interface SearchFilesResult {
  rows: FileRecord[]
  error?: string
}

/** FAZ 1.3c: renderer-localized CSV placeholders (native never hardcodes strings). */
export interface CsvExportLabels {
  /** Label used for carve records whose filesystem date is unknown. */
  noFsDate: string
  /** Placeholder for records with no date at all ('—'). */
  noDate: string
}

/** export-csv IPC response. */
export interface CsvExportResult {
  success: boolean
  canceled?: boolean
  path?: string
  rows?: number
  error?: string
}
