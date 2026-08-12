import { join } from 'path'

interface WolfEngine {
  getVersion(): string
  isAdministrator(): boolean
  listDrives(): Array<{
    index: number
    model: string
    serial: string
    sizeBytes: number
    sectorSize: number
    type: string
  }>
  initDatabase(path: string): boolean
  getFileCount(scanId: number): number
  getFilesPage(scanId: number, offset: number, limit: number): Array<{
    id: number
    parentId: number
    name: string
    extension: string
    path: string
    sizeBytes: number
    status: number
    confidence: number
    category: string
  }>
  getScanState(scanId: number): {
    id: number
    driveIndex: number
    scanType: string
    totalSectors: number
    scannedSectors: number
    status: number
  }
  readSectors(driveIndex: number, offset: number, size: number): {
    success: boolean
    bytesRead: number
    error: string
    data?: Buffer
  }
  getSmartStatus(driveIndex: number): {
    isValid: boolean
    driveModel?: string
    healthScore?: string
    temperatureC?: number
    powerOnHours?: number
    reallocatedSectors?: number
    pendingSectors?: number
  }
  startScan(drivePath: string, scanType: string, callback: (data: any) => void): boolean
  stopScan(): void
  startImaging(driveIndex: number, destPath: string, callback: (data: any) => void): boolean
  stopImaging(): void
}

let engine: WolfEngine | null = null

export function getEngine(): WolfEngine {
  if (!engine) {
    const nativePath = join(__dirname, '../../native/build/Release/wolf_engine.node')
    engine = require(nativePath) as WolfEngine
  }
  return engine
}

