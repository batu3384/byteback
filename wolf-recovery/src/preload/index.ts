import { contextBridge, ipcRenderer, IpcRendererEvent } from 'electron'

contextBridge.exposeInMainWorld('api', {
  getVersion: () => ipcRenderer.invoke('get-version'),
  isAdmin: () => ipcRenderer.invoke('is-admin'),
  listDrives: () => ipcRenderer.invoke('list-drives'),
  
  startScan: (driveIndex: number, scanType: string) => ipcRenderer.send('start-scan', driveIndex, scanType),
  stopScan: () => ipcRenderer.send('stop-scan'),
  
  onScanProgress: (callback: (data: { current: number, total: number }) => void) => {
    ipcRenderer.on('scan-progress', (event: IpcRendererEvent, data) => callback(data))
  },
  onScanFileFound: (callback: (data: { name: string, size: number }) => void) => {
    ipcRenderer.on('scan-file-found', (event: IpcRendererEvent, data) => callback(data))
  },
  
  removeAllScanListeners: () => {
    ipcRenderer.removeAllListeners('scan-progress')
    ipcRenderer.removeAllListeners('scan-file-found')
  }
})
