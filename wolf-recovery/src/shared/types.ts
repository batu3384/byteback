// Shared type definitions for renderer and main process.
export type {
  DriveInfo,
  FileRecord,
  DataRun,
  SmartStatus,
  ScanState,
  ScanSummary,
  RecoverResult,
  RaidAssemblyResult,
  BatchRecoverResult,
  PartitionInfo,
  TimelineEvent,
  TimelineResult,
  ProgressCallback,
  FileFoundCallback,
  ScanCompleteCallback,
  CaseInfo,
  NsrlStats,
} from './ipc-contract'

import type {
  DriveInfo,
  FileRecord,
  SmartStatus,
  ScanState,
  ScanSummary,
  RecoverResult,
  RaidAssemblyResult,
  BatchRecoverResult,
  PartitionInfo,
  TimelineEvent,
  TimelineResult,
  ProgressCallback,
  FileFoundCallback,
  ScanCompleteCallback,
  CaseInfo,
  NsrlStats,
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
      onScanComplete: (callback: ScanCompleteCallback) => () => void
      removeAllScanListeners: () => void

      startImaging: (driveIndex: number, destPath: string, format?: 'raw' | 'ewf') => void
      stopImaging: () => void
      onImagingProgress: (callback: (data: { current: number; total: number; md5?: string }) => void) => () => void

      getSmartStatus: (driveIndex: number) => Promise<SmartStatus>
      readHexData: (driveIndex: number, offset: number, size: number) => Promise<number[]>

      getFileCount: (scanId: number) => Promise<number>
      getFilesPage: (scanId: number, offset: number, limit: number) => Promise<FileRecord[]>
      searchFiles: (scanId: number, query: string, offset: number, limit: number, useRegex?: boolean, category?: string) => Promise<FileRecord[]>
      searchFileContent: (scanId: number, query: string, offset: number, limit: number) => Promise<FileRecord[]>
      startContentSearch: (scanId: number, query: string) => Promise<boolean>
      stopContentSearch: () => void
      onContentSearchProgress: (callback: (data: { current: number; total: number }) => void) => () => void
      onContentSearchMatch: (callback: (data: FileRecord) => void) => () => void
      onContentSearchComplete: (callback: (data: { status: number }) => void) => () => void
      getScanSummary: (scanId: number) => Promise<ScanSummary>
      getScanState: (scanId: number) => Promise<ScanState>
      getLatestScanId: () => Promise<number>
      getTimelineEvents: (scanId: number, offset: number, limit: number, eventTypeFilter?: string) => Promise<TimelineResult>
      getAuditLog: (maxLines?: number) => Promise<string[]>
      exportReportPdf: (html: string) => Promise<{ success: boolean; path?: string; error?: string; canceled?: boolean }>

      startWipe: (targetPath: string) => Promise<boolean>
      reconstructRaid: (driveIndices: number[], raidLevel: number) => Promise<RaidAssemblyResult>
      getRaidState: () => Promise<{ active: boolean; capacity: number; numDisks: number; level: number }>

      recoverFile: (
        driveIndex: number,
        fileRecord: FileRecord,
        destDir: string,
        scanId?: number,
      ) => Promise<RecoverResult>

      recoverFilesBatch: (
        driveIndex: number,
        fileRecords: FileRecord[],
        destDir: string,
        scanId?: number,
      ) => Promise<BatchRecoverResult>

      pickDirectory: () => Promise<string | null>
      pickSaveImage: (format: 'raw' | 'ewf') => Promise<string | null>
      getCaseInfo: () => Promise<CaseInfo>
      setCaseInfo: (info: Partial<CaseInfo>) => Promise<boolean>
      pickAndLoadNsrl: () => Promise<NsrlStats | null>
      getNsrlStats: () => Promise<NsrlStats>
      lookupNsrl: (md5Hex: string) => Promise<boolean>
    }
  }
}

export {}
