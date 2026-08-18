// Shared type definitions for renderer and main process.
export type {
  DriveInfo,
  FileRecord,
  DataRun,
  SmartStatus,
  ScanState,
  RecoverResult,
  RaidAssemblyResult,
  PartitionInfo,
  TimelineEvent,
  TimelineResult,
  ProgressCallback,
  FileFoundCallback,
} from './ipc-contract'

import type {
  DriveInfo,
  FileRecord,
  SmartStatus,
  ScanState,
  RecoverResult,
  RaidAssemblyResult,
  PartitionInfo,
  TimelineEvent,
  TimelineResult,
  ProgressCallback,
  FileFoundCallback,
} from './ipc-contract'

declare global {
  interface Window {
    api: {
      getVersion: () => Promise<string>
      isAdmin: () => Promise<boolean>
      listDrives: () => Promise<DriveInfo[]>
      listPartitions: (driveIndex: number) => Promise<PartitionInfo[]>

      startScan: (driveIndex: number, scanType: string) => Promise<number>
      stopScan: () => void
      onScanProgress: (callback: ProgressCallback) => () => void
      onScanFileFound: (callback: FileFoundCallback) => () => void
      removeAllScanListeners: () => void

      startImaging: (driveIndex: number, destPath: string, format?: 'raw' | 'ewf') => void
      stopImaging: () => void
      onImagingProgress: (callback: (data: { current: number; total: number; md5?: string }) => void) => () => void

      getSmartStatus: (driveIndex: number) => Promise<SmartStatus>
      readHexData: (driveIndex: number, offset: number, size: number) => Promise<number[]>

      getFileCount: (scanId: number) => Promise<number>
      getFilesPage: (scanId: number, offset: number, limit: number) => Promise<FileRecord[]>
      getScanState: (scanId: number) => Promise<ScanState>
      getLatestScanId: () => Promise<number>
      getTimelineEvents: (scanId: number, offset: number, limit: number, eventTypeFilter?: string) => Promise<TimelineResult>
      getAuditLog: (maxLines?: number) => Promise<string[]>
      exportReportPdf: (html: string) => Promise<{ success: boolean; path?: string; error?: string; canceled?: boolean }>

      startWipe: (targetPath: string) => Promise<boolean>
      reconstructRaid: (driveIndices: number[], raidLevel: number) => Promise<RaidAssemblyResult>

      recoverFile: (
        driveIndex: number,
        fileRecord: FileRecord,
        destDir: string,
        scanId?: number,
      ) => Promise<RecoverResult>

      pickDirectory: () => Promise<string | null>
      pickSaveImage: (format: 'raw' | 'ewf') => Promise<string | null>
    }
  }
}

export {}
