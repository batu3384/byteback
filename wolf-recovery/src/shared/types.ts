export interface DriveInfo {
  index: number
  model: string
  serial: string
  sizeBytes: number
  sectorSize: number
  type: 'HDD' | 'SSD' | 'USB' | 'Unknown'
}

export interface ScanProgress {
  scanId: number
  scannedSectors: number
  totalSectors: number
  percentComplete: number
  filesFound: number
  currentOffset: number
  status: 'running' | 'paused' | 'complete' | 'failed'
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
  source: string
}

export interface WolfAPI {
  getAppVersion(): Promise<string>
  getEngineVersion(): Promise<string>
  isAdministrator(): Promise<boolean>
  listDrives(): Promise<DriveInfo[]>
  startScan(driveIndex: number, scanType: string): void
  stopScan(): void
  onScanProgress(callback: (data: ScanProgress) => void): void
  onFileFound(callback: (data: FileRecord[]) => void): void
  onScanComplete(callback: (data: ScanProgress) => void): void
}

declare global {
  interface Window {
    api: WolfAPI
  }
}
