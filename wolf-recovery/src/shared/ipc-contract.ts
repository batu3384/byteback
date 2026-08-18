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
}

export type ProgressCallback = (data: { current: number; total: number; badSectors?: number[] }) => void
export type FileFoundCallback = (data: FileRecord & { type?: string; size?: number }) => void

export interface RaidAssemblyResult {
  success: boolean
  capacity: number
  numDisks: number
  error: string
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
