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
  onScanFileFound: (callback: (data: { name: string, size: number }) => void) => {
    const handler = (event: IpcRendererEvent, data: any) => callback(data)
    ipcRenderer.on('scan-file-found', handler)
    return () => ipcRenderer.removeListener('scan-file-found', handler)
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
  getScanState: (scanId: number) => ipcRenderer.invoke('get-scan-state', scanId),
  getLatestScanId: () => ipcRenderer.invoke('get-latest-scan-id'),
  getTimelineEvents: (scanId: number, offset: number, limit: number, filter?: string) => ipcRenderer.invoke('get-timeline-events', scanId, offset, limit, filter),
  getAuditLog: (maxLines?: number) => ipcRenderer.invoke('get-audit-log', maxLines),
  exportReportPdf: (html: string) => ipcRenderer.invoke('export-report-pdf', html),

  startWipe: (targetPath: string) => ipcRenderer.invoke('start-wipe', targetPath),
  reconstructRaid: (driveIndices: number[], raidLevel: number) =>
    ipcRenderer.invoke('reconstruct-raid', driveIndices, raidLevel),
  recoverFile: (driveIndex: number, fileRecord: any, destDir: string, scanId?: number) =>
    ipcRenderer.invoke('recover-file', driveIndex, fileRecord, destDir, scanId),
  pickDirectory: () => ipcRenderer.invoke('pick-directory'),
  pickSaveImage: (format: 'raw' | 'ewf') => ipcRenderer.invoke('pick-save-image', format),

  removeAllScanListeners: () => {
    ipcRenderer.removeAllListeners('scan-progress')
    ipcRenderer.removeAllListeners('scan-file-found')
  }
})

