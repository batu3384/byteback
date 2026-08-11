import { contextBridge, ipcRenderer, IpcRendererEvent } from 'electron'

contextBridge.exposeInMainWorld('api', {
  getVersion: () => ipcRenderer.invoke('get-version'),
  isAdmin: () => ipcRenderer.invoke('is-admin'),
  listDrives: () => ipcRenderer.invoke('list-drives'),
  
  startScan: (driveIndex: number, scanType: string) => ipcRenderer.send('start-scan', driveIndex, scanType),
  stopScan: () => ipcRenderer.send('stop-scan'),
  
  onScanProgress: (callback: (data: { current: number, total: number }) => void) => {
    const handler = (event: IpcRendererEvent, data: any) => callback(data)
    ipcRenderer.on('scan-progress', handler)
    return () => ipcRenderer.removeListener('scan-progress', handler)
  },
  onScanFileFound: (callback: (data: { name: string, size: number }) => void) => {
    const handler = (event: IpcRendererEvent, data: any) => callback(data)
    ipcRenderer.on('scan-file-found', handler)
    return () => ipcRenderer.removeListener('scan-file-found', handler)
  },
  
  startImaging: (driveIndex: number, destPath: string) => ipcRenderer.send('start-imaging', driveIndex, destPath),
  stopImaging: () => ipcRenderer.send('stop-imaging'),
  
  onImagingProgress: (callback: (data: { current: number, total: number }) => void) => {
    const handler = (event: IpcRendererEvent, data: any) => callback(data)
    ipcRenderer.on('imaging-progress', handler)
    return () => ipcRenderer.removeListener('imaging-progress', handler)
  },
  
  getSmartStatus: (driveIndex: number) => ipcRenderer.invoke('get-smart-status', driveIndex),
  readHexData: (driveIndex: number, offset: number, size: number) => ipcRenderer.invoke('read-hex-data', driveIndex, offset, size),

  getFileCount: (scanId: number) => ipcRenderer.invoke('get-file-count', scanId),
  getFilesPage: (scanId: number, offset: number, limit: number) => ipcRenderer.invoke('get-files-page', scanId, offset, limit),
  getScanState: (scanId: number) => ipcRenderer.invoke('get-scan-state', scanId),

  removeAllScanListeners: () => {
    ipcRenderer.removeAllListeners('scan-progress')
    ipcRenderer.removeAllListeners('scan-file-found')
  }
})

