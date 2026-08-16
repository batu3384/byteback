export interface DriveInfo {
  index: number
  model: string
  serial: string
  sizeBytes: number
  sectorSize: number
  type: string
}

export interface FileRecord {
  id: number
  parentId: number
  name: string
  extension: string
  path: string
  sizeBytes: number
  status: number
  confidence: number
  category: string
  startSector?: number
  endSector?: number
  runs?: Array<{ offset: number; length: number }>
}

export interface SmartStatus {
  isValid: boolean
  driveModel?: string
  healthScore?: string
  temperatureC?: number
  powerOnHours?: number
  reallocatedSectors?: number
  pendingSectors?: number
}

export interface ScanState {
  id: number
  driveIndex: number
  scanType: string
  totalSectors: number
  scannedSectors: number
  status: number
}

export interface RecoverResult {
  success: boolean
  destPath?: string
  bytesRecovered?: number
  md5Hash?: string
  error?: string
}

export type ProgressCallback = (data: { current: number; total: number }) => void
export type FileFoundCallback = (data: { name: string; size: number }) => void

/** Result of assembling a virtual RAID array over physical disks. */
export interface RaidAssemblyResult {
  success: boolean
  /** Logical capacity of the assembled array in bytes (0 on failure). */
  capacity: number
  /** Number of member disks in the array. */
  numDisks: number
  /** Human-readable failure reason; empty string on success. */
  error: string
}

/** A partition parsed from a drive's MBR or GPT table. */
export interface PartitionInfo {
  type: string
  startSector: number
  sizeInSectors: number
  label: string
  isActive: boolean
}

interface WolfEngine {
  getVersion(): string
  isAdministrator(): boolean
  listDrives(): DriveInfo[]
  listPartitions(driveIndex: number): PartitionInfo[]
  initDatabase(path: string): boolean
  getFileCount(scanId: number): number
  getFilesPage(scanId: number, offset: number, limit: number): FileRecord[]
  getScanState(scanId: number): ScanState
  readSectors(driveIndex: number, offset: number, size: number): {
    success: boolean
    bytesRead: number
    error: string
    data?: Buffer
  }
  getSmartStatus(driveIndex: number): SmartStatus
  startScan(drivePath: string, scanType: string, callback: (data: any) => void): boolean
  stopScan(): void
  startImaging(
    driveIndex: number,
    destPath: string,
    callback: (data: any) => void,
    format?: 'raw' | 'ewf',
  ): boolean
  stopImaging(): void
  startWipe(targetPath: string): Promise<boolean>
  reconstructRaid(driveIndices: number[], raidLevel: number): RaidAssemblyResult
  recoverFile(
    driveIndex: number,
    fileRecord: FileRecord,
    destDir: string,
  ): RecoverResult
}

let engine: WolfEngine | null = null
let loadError: Error | null = null

/**
 * Returns the cached native engine instance.
 * Loads the native addon on first call. If the addon is missing or fails to
 * load, every subsequent call returns the same captured error so callers get a
 * consistent, actionable message instead of a raw stack from require().
 *
 * The addon path is a static relative literal resolved against the bundled
 * main-process __dirname — it is never derived from user or IPC input, so it
 * cannot be redirected to load an arbitrary module.
 */
export function getEngine(): WolfEngine {
  if (engine) return engine
  if (loadError) throw loadError

  try {
    // Static literal path — electron-vite bundles main into out/main/, so the
    // relative reference resolves to <repo>/native/build/Release/wolf_engine.node.
    // Using a literal (not a variable) keeps the loader bound to this single
    // known artifact and cannot be influenced by runtime input.
    engine = require('../../native/build/Release/wolf_engine.node') as WolfEngine
    if (!engine || typeof engine.getVersion !== 'function') {
      throw new Error('Native addon loaded but did not expose the expected WolfEngine API')
    }
    return engine
  } catch (e: any) {
    loadError = new Error(
      `Native engine yüklenemedi. Önce "npm run build:native" komutunu çalıştırdığınızdan emin olun. Detay: ${e?.message ?? e}`,
    )
    throw loadError
  }
}

/** True if the native engine loaded successfully at least once. */
export function isEngineAvailable(): boolean {
  try {
    getEngine()
    return true
  } catch {
    return false
  }
}
