// Shared type definitions for renderer and main process.
export type {
  DriveInfo,
  FileRecord,
  DataRun,
  SmartStatus,
  ScanState,
  ScanSummary,
  FileListFilter,
  RecoverResult,
  FilePreviewResult,
  RaidAssemblyResult,
  BatchRecoverResult,
  PartitionInfo,
  ScanOptions,
  ResolvedVolume,
  TimelineEvent,
  TimelineResult,
  ProgressCallback,
  FileFoundCallback,
  ScanCompleteCallback,
  CaseInfo,
  NsrlStats,
  SearchFilesResult,
} from './ipc-contract'

export type { HexReadResult, IpcOkResult } from './ipc-result'

import type {
  DriveInfo,
  FileRecord,
  SmartStatus,
  ScanState,
  ScanSummary,
  FileListFilter,
  RecoverResult,
  FilePreviewResult,
  RaidAssemblyResult,
  BatchRecoverResult,
  PartitionInfo,
  ScanOptions,
  ResolvedVolume,
  TimelineEvent,
  TimelineResult,
  ProgressCallback,
  FileFoundCallback,
  ScanCompleteCallback,
  CaseInfo,
  NsrlStats,
  SearchFilesResult,
} from './ipc-contract'
import type { HexReadResult, IpcOkResult } from './ipc-result'

declare global {
  interface Window {
    api: {
      getVersion: () => Promise<string>
      getDbStatus: () => Promise<{ ready: boolean; error?: string }>
      isAdmin: () => Promise<boolean>
      listDrives: () => Promise<DriveInfo[]>
      listPartitions: (driveIndex: number) => Promise<PartitionInfo[]>
      resolveVolume: (letter: string) => Promise<ResolvedVolume | null>
      listVolumeLetters: () => Promise<string[]>

      startScan: (driveIndex: number, scanType: string, scanOptions?: ScanOptions) => Promise<number>
      stopScan: () => void
      onScanProgress: (callback: ProgressCallback) => () => void
      onScanFileFound: (callback: FileFoundCallback) => () => void
      onScanComplete: (callback: ScanCompleteCallback) => () => void
      removeAllScanListeners: () => void

      startImaging: (driveIndex: number, destPath: string, format?: 'raw' | 'ewf') => void
      stopImaging: () => void
      onImagingProgress: (callback: (data: { current: number; total: number; md5?: string }) => void) => () => void

      getSmartStatus: (driveIndex: number) => Promise<SmartStatus>
      readHexData: (driveIndex: number, offset: number, size: number) => Promise<HexReadResult>

      getFileCount: (scanId: number, filter?: FileListFilter) => Promise<number>
      getFilesPage: (scanId: number, offset: number, limit: number, filter?: FileListFilter) => Promise<FileRecord[]>
      searchFiles: (scanId: number, query: string, offset: number, limit: number, useRegex?: boolean, category?: string) => Promise<SearchFilesResult>
      searchFileContent: (scanId: number, query: string, offset: number, limit: number) => Promise<SearchFilesResult>
      startContentSearch: (scanId: number, query: string) => Promise<IpcOkResult>
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

      pickAndWipeFile: () => Promise<IpcOkResult>
      pickAndWipeFreeSpace: () => Promise<IpcOkResult>
      wipePhysicalDrive: (driveIndex: number, typedSerial: string, confirmPhrase: string) => Promise<IpcOkResult>
      setBitLockerFvek: (hex: string) => Promise<boolean>
      setBitLockerRecoveryPassword: (driveIndex: number, password: string) => Promise<string>
      setBitLockerPassword: (driveIndex: number, password: string) => Promise<string>
      reconstructRaid: (driveIndices: number[], raidLevel: number) => Promise<RaidAssemblyResult>
      failRaidDisk: (diskIndex: number) => Promise<boolean>
      getRaidState: () => Promise<{ active: boolean; capacity: number; numDisks: number; level: number; failedDisks?: number[] }>

      recoverFile: (
        driveIndex: number,
        fileId: number,
        destDir: string,
        scanId: number,
      ) => Promise<RecoverResult>

      recoverFilesBatch: (
        driveIndex: number,
        fileIds: number[],
        destDir: string,
        scanId: number,
      ) => Promise<BatchRecoverResult>

      readFilePreview: (
        driveIndex: number,
        scanId: number,
        fileId: number,
      ) => Promise<FilePreviewResult>

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
