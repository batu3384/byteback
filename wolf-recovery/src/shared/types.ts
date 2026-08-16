// Shared type definitions for renderer and main process.
// Re-exports the canonical types from native-bridge so there is a single
// source of truth for the IPC contract and native API surface.

export type {
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
} from '../main/native-bridge'

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
} from '../main/native-bridge'

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
      getTimelineEvents: (scanId: number, offset: number, limit: number, eventTypeFilter?: string) => Promise<TimelineResult>

      startWipe: (targetPath: string) => Promise<boolean>
      reconstructRaid: (driveIndices: number[], raidLevel: number) => Promise<RaidAssemblyResult>

      // File recovery: returns {success, destPath, bytesRecovered, md5Hash}
      recoverFile: (
        driveIndex: number,
        fileRecord: FileRecord,
        destDir: string,
      ) => Promise<RecoverResult>

      // Native directory picker (main-process dialog). Resolves to the chosen
      // path or null if the user cancelled.
      pickDirectory: () => Promise<string | null>
    }
  }
}

export {}
