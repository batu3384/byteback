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
}

let engine: WolfEngine | null = null

export function getEngine(): WolfEngine {
  if (!engine) {
    const nativePath = join(__dirname, '../../native/build/Release/wolf_engine.node')
    engine = require(nativePath) as WolfEngine
  }
  return engine
}

