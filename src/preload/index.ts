import { contextBridge, ipcRenderer, IpcRendererEvent } from 'electron'

contextBridge.exposeInMainWorld('api', {
  getVersion: () => ipcRenderer.invoke('get-version'),
  getCarveSignatureCount: () => ipcRenderer.invoke('get-carve-signature-count') as Promise<number>,
  getDbStatus: () => ipcRenderer.invoke('get-db-status') as Promise<{ ready: boolean; error?: string }>,
  isAdmin: () => ipcRenderer.invoke('is-admin'),
  listDrives: () => ipcRenderer.invoke('list-drives'),
  listPartitions: (driveIndex: number) => ipcRenderer.invoke('list-partitions', driveIndex),
  resolveVolume: (letter: string) => ipcRenderer.invoke('resolve-volume', letter),
  listVolumeLetters: () => ipcRenderer.invoke('list-volume-letters'),
  pickScanImage: () => ipcRenderer.invoke('pick-scan-image') as Promise<string | null>,
  pickRaidMemberImages: () => ipcRenderer.invoke('pick-raid-member-images') as Promise<string[]>,
  
  startScan: (driveIndex: number, scanType: string, scanOptions?: import('../shared/ipc-contract').ScanOptions) =>
    ipcRenderer.invoke('start-scan', driveIndex, scanType, scanOptions),
  stopScan: () => ipcRenderer.send('stop-scan'),

  seedScanFixture: (files: Array<Record<string, unknown>>, imagePath?: string) =>
    ipcRenderer.invoke('seed-scan-fixture', files, imagePath),
  seedImageDest: (destPath: string) => ipcRenderer.invoke('seed-image-dest', destPath),
  
  onScanProgress: (callback: (data: { scanId?: number, current: number, total: number, badSectors?: number[], phase?: string, phaseCurrent?: number, phaseTotal?: number }) => void) => {
    const handler = (event: IpcRendererEvent, data: any) => callback(data)
    ipcRenderer.on('scan-progress', handler)
    return () => ipcRenderer.removeListener('scan-progress', handler)
  },
  onScanComplete: (callback: (data: { scanId: number; status: number }) => void) => {
    const handler = (event: IpcRendererEvent, data: any) => callback(data)
    ipcRenderer.on('scan-complete', handler)
    return () => ipcRenderer.removeListener('scan-complete', handler)
  },
  
  startImaging: (driveIndex: number, destPath: string, format?: 'raw' | 'ewf', volumePath?: string) =>
    ipcRenderer.send('start-imaging', driveIndex, destPath, format, volumePath),
  stopImaging: () => ipcRenderer.send('stop-imaging'),

  onImagingProgress: (callback: (data: { current: number, total: number, md5?: string, error?: string, status?: 'cancelled' }) => void) => {
    const handler = (event: IpcRendererEvent, data: any) => callback(data)
    ipcRenderer.on('imaging-progress', handler)
    return () => ipcRenderer.removeListener('imaging-progress', handler)
  },
  
  getSmartStatus: (driveIndex: number) => ipcRenderer.invoke('get-smart-status', driveIndex),
  getDataPaths: () => ipcRenderer.invoke('get-data-paths'),
  readHexData: (driveIndex: number, offset: number, size: number, volumePath?: string) => ipcRenderer.invoke('read-hex-data', driveIndex, offset, size, volumePath),
  searchHex: (driveIndex: number, needle: number[], maxHits?: number, volumePath?: string) =>
    ipcRenderer.invoke('search-hex', driveIndex, needle, maxHits, volumePath),
  getMftRecord: (driveIndex: number, mftRef: number, volumePath?: string) =>
    ipcRenderer.invoke('get-mft-record', driveIndex, mftRef, volumePath),
  getHexMarks: () => ipcRenderer.invoke('get-hex-marks'),
  setHexMarks: (marks: import('../shared/hex-marks').HexMark[]) =>
    ipcRenderer.invoke('set-hex-marks', marks),

  getFileCount: (scanId: number, filter?: import('../shared/ipc-contract').FileListFilter) => ipcRenderer.invoke('get-file-count', scanId, filter),
  getFilesPage: (scanId: number, offset: number, limit: number, filter?: import('../shared/ipc-contract').FileListFilter) => ipcRenderer.invoke('get-files-page', scanId, offset, limit, filter),
  hashEmptyContent: (scanId: number) =>
    ipcRenderer.invoke('hash-empty-content', scanId) as Promise<{ hashed: number; error?: string }>,
  searchFiles: (scanId: number, query: string, offset: number, limit: number, useRegex?: boolean, category?: string) =>
    ipcRenderer.invoke('search-files', scanId, query, offset, limit, useRegex, category),
  searchFileContent: (scanId: number, query: string, offset: number, limit: number, useRegex?: boolean) =>
    ipcRenderer.invoke('search-file-content', scanId, query, offset, limit, !!useRegex),
  startContentSearch: (scanId: number, query: string, useRegex?: boolean) =>
    ipcRenderer.invoke('start-content-search', scanId, query, !!useRegex),
  stopContentSearch: () => ipcRenderer.send('stop-content-search'),
  onContentSearchProgress: (callback: (data: { current: number; total: number }) => void) => {
    const handler = (_event: IpcRendererEvent, data: any) => callback(data)
    ipcRenderer.on('content-search-progress', handler)
    return () => ipcRenderer.removeListener('content-search-progress', handler)
  },
  onContentSearchMatch: (callback: (data: any) => void) => {
    const handler = (_event: IpcRendererEvent, data: any) => callback(data)
    ipcRenderer.on('content-search-match', handler)
    return () => ipcRenderer.removeListener('content-search-match', handler)
  },
  onContentSearchComplete: (callback: (data: { status: number }) => void) => {
    const handler = (_event: IpcRendererEvent, data: any) => callback(data)
    ipcRenderer.on('content-search-complete', handler)
    return () => ipcRenderer.removeListener('content-search-complete', handler)
  },
  getScanSummary: (scanId: number) => ipcRenderer.invoke('get-scan-summary', scanId),
  getScanState: (scanId: number) => ipcRenderer.invoke('get-scan-state', scanId),
  getLatestScanId: () => ipcRenderer.invoke('get-latest-scan-id'),
  getLatestUsableScanId: () => ipcRenderer.invoke('get-latest-usable-scan-id'),
  resetScanDatabase: () => ipcRenderer.invoke('reset-scan-database'),
  getTimelineEvents: (scanId: number, offset: number, limit: number, filter?: string) => ipcRenderer.invoke('get-timeline-events', scanId, offset, limit, filter),
  getAuditLog: (maxLines?: number) => ipcRenderer.invoke('get-audit-log', maxLines),
  verifyAuditLog: () =>
    ipcRenderer.invoke('verify-audit-log') as Promise<{ ok: boolean; entries: number; brokenAt: number; detail: string }>,
  getSessionLog: (maxLines?: number) =>
    ipcRenderer.invoke('get-session-log', maxLines) as Promise<{ path: string; lines: string[]; summary: string; code: string }>,
  exportReportPdf: (html: string) => ipcRenderer.invoke('export-report-pdf', html),

  pickAndWipeFile: () => ipcRenderer.invoke('pick-and-wipe-file'),
  pickAndWipeFreeSpace: () => ipcRenderer.invoke('pick-and-wipe-freespace'),
  wipePhysicalDrive: (driveIndex: number, typedSerial: string, confirmPhrase: string) =>
    ipcRenderer.invoke('wipe-physical-drive', driveIndex, typedSerial, confirmPhrase),
  setBitLockerFvek: (hex: string) => ipcRenderer.invoke('set-bitlocker-fvek', hex),
  setBitLockerRecoveryPassword: (driveIndex: number, password: string) =>
    ipcRenderer.invoke('set-bitlocker-recovery-password', driveIndex, password),
  setBitLockerPassword: (driveIndex: number, password: string) =>
    ipcRenderer.invoke('set-bitlocker-password', driveIndex, password),
  setLuksPassword: (driveIndex: number, password: string) =>
    ipcRenderer.invoke('set-luks-password', driveIndex, password),
  detectRaid: (driveIndices: number[]) => ipcRenderer.invoke('detect-raid', driveIndices),
  detectRaidImages: (imagePaths: string[]) => ipcRenderer.invoke('detect-raid-images', imagePaths),
  reconstructRaid: (driveIndices: number[], raidLevel: number, blockSize: number, dataOffsetSectors?: number, raid5Algorithm?: number) =>
    ipcRenderer.invoke('reconstruct-raid', driveIndices, raidLevel, blockSize, dataOffsetSectors, raid5Algorithm),
  reconstructRaidImages: (imagePaths: string[], raidLevel: number, blockSize: number, dataOffsetSectors?: number, raid5Algorithm?: number) =>
    ipcRenderer.invoke('reconstruct-raid-images', imagePaths, raidLevel, blockSize, dataOffsetSectors, raid5Algorithm),
  assembleLvm: (driveIndices: number[]) => ipcRenderer.invoke('assemble-lvm', driveIndices),
  assembleLvmImages: (imagePaths: string[]) => ipcRenderer.invoke('assemble-lvm-images', imagePaths),
  assembleLdm: (driveIndices: number[]) => ipcRenderer.invoke('assemble-ldm', driveIndices),
  assembleLdmImages: (imagePaths: string[]) => ipcRenderer.invoke('assemble-ldm-images', imagePaths),
  failRaidDisk: (diskIndex: number) => ipcRenderer.invoke('fail-raid-disk', diskIndex),
  getRaidState: () => ipcRenderer.invoke('get-raid-state'),
  recoverFile: (driveIndex: number, fileId: number, destDir: string, scanId: number, preservePaths?: boolean) =>
    ipcRenderer.invoke('recover-file', driveIndex, fileId, destDir, scanId, preservePaths),
  recoverFilesBatch: (driveIndex: number, fileIds: number[], destDir: string, scanId: number, preservePaths?: boolean) =>
    ipcRenderer.invoke('recover-files-batch', driveIndex, fileIds, destDir, scanId, preservePaths),
  scanLostPartitions: (driveIndex: number, stepSectors?: number) =>
    ipcRenderer.invoke('scan-lost-partitions', driveIndex, stepSectors),
  pickAndSetSignatureOverlay: () => ipcRenderer.invoke('pick-and-set-signature-overlay'),
  readFilePreview: (driveIndex: number, scanId: number, fileId: number) =>
    ipcRenderer.invoke('read-file-preview', driveIndex, scanId, fileId),
  pickDirectory: () => ipcRenderer.invoke('pick-directory'),
  pickSaveImage: (format: 'raw' | 'ewf') => ipcRenderer.invoke('pick-save-image', format),
  getCaseInfo: () => ipcRenderer.invoke('get-case-info'),
  setCaseInfo: (info: Record<string, string>) => ipcRenderer.invoke('set-case-info', info),
  pickAndLoadNsrl: () => ipcRenderer.invoke('pick-and-load-nsrl'),
  getNsrlStats: () => ipcRenderer.invoke('get-nsrl-stats'),
  lookupNsrl: (md5Hex: string) => ipcRenderer.invoke('lookup-nsrl', md5Hex),

  // FAZ 1.3c: dialog-arbitrated CSV export — the renderer never sends a path.
  exportCsv: (scanId: number, filter?: import('../shared/ipc-contract').FileListFilter,
              header?: string[], labels?: import('../shared/ipc-contract').CsvExportLabels,
              suggestedName?: string) =>
    ipcRenderer.invoke('export-csv', scanId, filter, header, labels, suggestedName) as
      Promise<import('../shared/ipc-contract').CsvExportResult>,

  // FAZ 1.3b: gallery thumbnail disk cache (L2) behind the thumb:// protocol.
  getThumbUrl: (fileId: number, scanId: number) =>
    ipcRenderer.invoke('get-thumb-url', fileId, scanId) as Promise<string | null>,
  putThumb: (fileId: number, scanId: number, mime: string, base64: string) =>
    ipcRenderer.invoke('put-thumb', fileId, scanId, mime, base64) as Promise<string | null>,

  removeAllScanListeners: () => {
    ipcRenderer.removeAllListeners('scan-progress')
    ipcRenderer.removeAllListeners('scan-complete')
  }
})

