import { contextBridge, ipcRenderer, IpcRendererEvent } from 'electron'

contextBridge.exposeInMainWorld('api', {
  getVersion: () => ipcRenderer.invoke('get-version'),
  isAdmin: () => ipcRenderer.invoke('is-admin'),
  listDrives: () => ipcRenderer.invoke('list-drives'),
  listPartitions: (driveIndex: number) => ipcRenderer.invoke('list-partitions', driveIndex),
  
  startScan: (driveIndex: number, scanType: string) => ipcRenderer.invoke('start-scan', driveIndex, scanType),
  stopScan: () => ipcRenderer.send('stop-scan'),
  
  onScanProgress: (callback: (data: { current: number, total: number, badSectors?: number[] }) => void) => {
    const handler = (event: IpcRendererEvent, data: any) => callback(data)
    ipcRenderer.on('scan-progress', handler)
    return () => ipcRenderer.removeListener('scan-progress', handler)
  },
  onScanFileFound: (callback: (data: any) => void) => {
    const handler = (event: IpcRendererEvent, data: any) => callback(data)
    ipcRenderer.on('scan-file-found', handler)
    return () => ipcRenderer.removeListener('scan-file-found', handler)
  },
  onScanComplete: (callback: (data: { scanId: number; status: number }) => void) => {
    const handler = (event: IpcRendererEvent, data: any) => callback(data)
    ipcRenderer.on('scan-complete', handler)
    return () => ipcRenderer.removeListener('scan-complete', handler)
  },
  
  startImaging: (driveIndex: number, destPath: string, format?: 'raw' | 'ewf') =>
    ipcRenderer.send('start-imaging', driveIndex, destPath, format),
  stopImaging: () => ipcRenderer.send('stop-imaging'),

  onImagingProgress: (callback: (data: { current: number, total: number, md5?: string }) => void) => {
    const handler = (event: IpcRendererEvent, data: any) => callback(data)
    ipcRenderer.on('imaging-progress', handler)
    return () => ipcRenderer.removeListener('imaging-progress', handler)
  },
  
  getSmartStatus: (driveIndex: number) => ipcRenderer.invoke('get-smart-status', driveIndex),
  readHexData: (driveIndex: number, offset: number, size: number) => ipcRenderer.invoke('read-hex-data', driveIndex, offset, size),

  getFileCount: (scanId: number) => ipcRenderer.invoke('get-file-count', scanId),
  getFilesPage: (scanId: number, offset: number, limit: number) => ipcRenderer.invoke('get-files-page', scanId, offset, limit),
  searchFiles: (scanId: number, query: string, offset: number, limit: number, useRegex?: boolean, category?: string) =>
    ipcRenderer.invoke('search-files', scanId, query, offset, limit, useRegex, category),
  searchFileContent: (scanId: number, query: string, offset: number, limit: number) =>
    ipcRenderer.invoke('search-file-content', scanId, query, offset, limit),
  startContentSearch: (scanId: number, query: string) =>
    ipcRenderer.invoke('start-content-search', scanId, query),
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
  getTimelineEvents: (scanId: number, offset: number, limit: number, filter?: string) => ipcRenderer.invoke('get-timeline-events', scanId, offset, limit, filter),
  getAuditLog: (maxLines?: number) => ipcRenderer.invoke('get-audit-log', maxLines),
  exportReportPdf: (html: string) => ipcRenderer.invoke('export-report-pdf', html),

  startWipe: (targetPath: string) => ipcRenderer.invoke('start-wipe', targetPath),
  reconstructRaid: (driveIndices: number[], raidLevel: number) =>
    ipcRenderer.invoke('reconstruct-raid', driveIndices, raidLevel),
  getRaidState: () => ipcRenderer.invoke('get-raid-state'),
  recoverFile: (driveIndex: number, fileRecord: any, destDir: string, scanId?: number) =>
    ipcRenderer.invoke('recover-file', driveIndex, fileRecord, destDir, scanId),
  recoverFilesBatch: (driveIndex: number, fileRecords: any[], destDir: string, scanId?: number) =>
    ipcRenderer.invoke('recover-files-batch', driveIndex, fileRecords, destDir, scanId),
  pickDirectory: () => ipcRenderer.invoke('pick-directory'),
  pickSaveImage: (format: 'raw' | 'ewf') => ipcRenderer.invoke('pick-save-image', format),
  getCaseInfo: () => ipcRenderer.invoke('get-case-info'),
  setCaseInfo: (info: Record<string, string>) => ipcRenderer.invoke('set-case-info', info),
  pickAndLoadNsrl: () => ipcRenderer.invoke('pick-and-load-nsrl'),
  getNsrlStats: () => ipcRenderer.invoke('get-nsrl-stats'),
  lookupNsrl: (md5Hex: string) => ipcRenderer.invoke('lookup-nsrl', md5Hex),

  removeAllScanListeners: () => {
    ipcRenderer.removeAllListeners('scan-progress')
    ipcRenderer.removeAllListeners('scan-file-found')
    ipcRenderer.removeAllListeners('scan-complete')
  }
})

