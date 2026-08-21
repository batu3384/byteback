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
}

export interface FilePreviewResult {
  success: boolean
  error?: string
  kind?: 'image' | 'text' | 'pdf' | 'binary' | string
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
  includeDuplicates?: boolean
  includeDiscovery?: boolean
}

export type ProgressCallback = (data: {
  current: number
  total: number
  badSectors?: number[]
  phase?: string
}) => void
export type ScanCompleteCallback = (data: { scanId: number; status: number }) => void
export type FileFoundCallback = (data: FileRecord & { type?: string; size?: number }) => void

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
}

export interface BatchRecoverResult {
  succeeded: number
  failed: number
  results: RecoverResult[]
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

/** Optional partition scope for startScan (whole disk when omitted). */
export interface ScanOptions {
  partitionIndex?: number
  partitionStartSector?: number
  partitionSizeInSectors?: number
  resumeScanId?: number
  /** Required for deep/full_carve on SSD after TRIM warning. */
  allowSsdDeepScan?: boolean
}

/** Logical drive letter resolved to PhysicalDrive + partition extent. */
export interface ResolvedVolume {
  driveIndex: number
  startSector: number
  sizeSectors: number
  fsType: string
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
