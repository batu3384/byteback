export type {
  DriveInfo,
  FileRecord,
  DataRun,
  SmartStatus,
  ScanState,
  RecoverResult,
  RaidAssemblyResult,
  RaidDetection,
  PartitionInfo,
  TimelineEvent,
  TimelineResult,
  ProgressCallback,
} from '../shared/ipc-contract'

import { existsSync } from 'fs'
import { nativeAddonCandidates, signaturesDirCandidates } from './native-addon-path'
import { APP_VERSION } from '../shared/app-version'
import type {
  DriveInfo,
  FileRecord,
  PartitionInfo,
  ResolvedVolume,
  ScanState,
  TimelineResult,
  ScanSummary,
  SmartStatus,
  RaidAssemblyResult,
  RaidDetection,
  RecoverResult,
  FilePreviewResult,
} from '../shared/ipc-contract'

interface BytebackEngine {
  getVersion(): string
  getCarveSignatureCount(): number
  isAdministrator(): boolean
  listDrives(): DriveInfo[]
  listPartitions(driveIndex: number): PartitionInfo[]
  resolveVolume(letter: string): ResolvedVolume | null
  listVolumeLetters(): string[]
  initDatabase(path: string): boolean
  getFileCount(scanId: number, filter?: import('../shared/ipc-contract').FileListFilter): number
  getFilesPage(scanId: number, offset: number, limit: number, filter?: import('../shared/ipc-contract').FileListFilter): FileRecord[]
  searchFiles(scanId: number, query: string, offset: number, limit: number, useRegex?: boolean, category?: string): FileRecord[]
  searchFileContent(scanId: number, query: string, offset: number, limit: number): FileRecord[]
  startContentSearch(scanId: number, query: string, callback: (data: any) => void): boolean
  stopContentSearch(): void
  getScanSummary(scanId: number): ScanSummary
  getScanState(scanId: number): ScanState
  getLatestScanId(): number
  getLatestUsableScanId(): number
  resetScanDatabase(): boolean
  getTimelineEvents(scanId: number, offset: number, limit: number, eventTypeFilter?: string): TimelineResult
  getAuditLog(maxLines?: number): string[]
  verifyAuditLog(): { ok: boolean; entries: number; brokenAt: number; detail: string }
  readSectors(driveIndex: number, offset: number, size: number): {
    success: boolean
    bytesRead: number
    error: string
    paddedZeros?: boolean
    data?: Buffer
  }
  getSmartStatus(driveIndex: number): SmartStatus
  startScan(
    drivePath: string,
    scanType: string,
    callbackOrOptions: ((data: unknown) => void) | Record<string, unknown> | import('../shared/ipc-contract').ScanOptions,
    callback?: (data: unknown) => void,
  ): number
  stopScan(): void
  isScanActive(): boolean
  startImaging(
    driveIndex: number,
    destPath: string,
    callback: (data: any) => void,
    format?: 'raw' | 'ewf',
  ): boolean
  stopImaging(): void
  startWipe(targetPath: string): Promise<boolean>
  setBitLockerFvek(hex: string): boolean
  setBitLockerRecoveryPassword(driveIndex: number, password: string): string
  setBitLockerPassword(driveIndex: number, password: string): string
  startPhysicalWipe(driveIndex: number, typedSerial: string): Promise<boolean>
  detectRaid(driveIndices: number[]): RaidDetection
  reconstructRaid(driveIndices: number[], raidLevel: number): RaidAssemblyResult
  failRaidDisk(diskIndex: number): boolean
  // Synced with the native emission (bridge_wipe.cpp GetRaidState): both index
  // arrays are always present, empty when the array is inactive.
  getRaidState(): { active: boolean; capacity: number; numDisks: number; level: number; failedDisks: number[]; memberDriveIndices: number[] }
  recoverFile(
    driveIndex: number,
    fileId: number,
    destDir: string,
    scanId: number,
    preservePaths?: boolean,
  ): Promise<RecoverResult>
  recoverFilesBatch(
    driveIndex: number,
    fileIds: number[],
    destDir: string,
    scanId: number,
    preservePaths?: boolean,
  ): Promise<{ succeeded: number; failed: number; results: RecoverResult[] }>
  scanLostPartitions(
    driveIndex: number,
    stepSectors?: number,
  ): Promise<Array<{ startSector: number; sizeSectors: number; fs: string }>>
  setSignatureOverlay(path: string): boolean
  readFilePreview(driveIndex: number, scanId: number, fileId: number): FilePreviewResult
  getCaseInfo(): {
    caseNumber: string
    investigator: string
    agency: string
    notes: string
    createdAt: number
    updatedAt: number
  }
  setCaseInfo(info: {
    caseNumber?: string
    investigator?: string
    agency?: string
    notes?: string
  }): boolean
  loadNsrl(path: string): { ok: boolean; count: number; path: string }
  lookupNsrl(md5Hex: string): boolean
  getNsrlStats(): { count: number; path: string }
  setSignaturesDir(dir: string): boolean
  /** e2e-only: seed a completed scan + records into the app DB; returns scanId. */
  seedScanFixture(files: Array<Record<string, unknown>>): number
}

let engine: BytebackEngine | null = null
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
export function getEngine(): BytebackEngine {
  if (engine) return engine
  if (loadError) throw loadError

  try {
    const candidates = nativeAddonCandidates(__dirname, process.resourcesPath)
    const addonPath = candidates.find((p) => existsSync(p)) ?? candidates[0]
    engine = require(addonPath) as BytebackEngine
    if (!engine || typeof engine.getVersion !== 'function') {
      throw new Error('Native addon loaded but did not expose the expected BytebackEngine API')
    }
    // CA-049: an engine/JS version skew must be visible, not silent — reports
    // stamp the JS version while the data comes from the native engine.
    const nativeVersion = engine.getVersion()
    if (nativeVersion && nativeVersion !== APP_VERSION) {
      console.warn(`[byteback] version skew: JS ${APP_VERSION} vs native engine ${nativeVersion}`)
    }
    // CA-010: std::ifstream cannot read inside app.asar, and the packaged CWD
    // is not the install dir — hand the carver an absolute directory holding
    // the (asarUnpacked) signature JSONs.
    const sigDir = signaturesDirCandidates(__dirname, process.resourcesPath).find((p) =>
      existsSync(p),
    )
    if (sigDir && typeof engine.setSignaturesDir === 'function') {
      engine.setSignaturesDir(sigDir)
    }
    return engine
  } catch (e: any) {
    // If the addon loaded but failed validation (getVersion check), `engine` is
    // already assigned — reset it so callers get loadError, not a broken engine.
    engine = null
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
