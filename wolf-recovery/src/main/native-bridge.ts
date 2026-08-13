import { join } from 'path'

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

interface WolfEngine {
  getVersion(): string
  isAdministrator(): boolean
  listDrives(): DriveInfo[]
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
  startImaging(driveIndex: number, destPath: string, callback: (data: any) => void): boolean
  stopImaging(): void
  startWipe(targetPath: string): Promise<boolean>
  reconstructRaid(driveIndices: number[], raidLevel: number): boolean
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
 */
export function getEngine(): WolfEngine {
  if (engine) return engine
  if (loadError) throw loadError

  try {
    const nativePath = join(__dirname, '../../native/build/Release/wolf_engine.node')
    engine = require(nativePath) as WolfEngine
    return engine
  } catch (e: any) {
    loadError = new Error(
      `Native engine yüklenemedi ("${join(__dirname, '../../native/build/Release/wolf_engine.node')}"). ` +
        `Önce "npm run build:native" komutunu çalıştırdığınızdan emin olun. Detay: ${e?.message ?? e}`,
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
