export interface DriveInfo {
  index: number
  model: string
  serial: string
  sizeBytes: number
  sectorSize: number
  type: string
}

declare global {
  interface Window {
    api: {
      getVersion: () => Promise<string>
      isAdmin: () => Promise<boolean>
      listDrives: () => Promise<DriveInfo[]>
      
      startScan: (driveIndex: number, scanType: string) => void
      stopScan: () => void
      onScanProgress: (callback: (data: { current: number, total: number }) => void) => void
      onScanFileFound: (callback: (data: { name: string, size: number }) => void) => void
      removeAllScanListeners: () => void
      getSmartStatus: (driveIndex: number) => Promise<any>
      readHexData: (driveIndex: number, offset: number, size: number) => Promise<any>
    }
  }
}

