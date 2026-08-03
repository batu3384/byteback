import { contextBridge, ipcRenderer } from 'electron'

const api = {
  getAppVersion: (): Promise<string> => ipcRenderer.invoke('get-app-version'),

  // Disk operations (will be connected in Task 5)
  listDrives: (): Promise<unknown[]> => ipcRenderer.invoke('list-drives'),
  startScan: (driveIndex: number, scanType: string): void =>
    ipcRenderer.send('start-scan', { driveIndex, scanType }),
  stopScan: (): void => ipcRenderer.send('stop-scan'),

  // Event listeners
  onScanProgress: (callback: (data: unknown) => void): void => {
    ipcRenderer.on('scan-progress', (_event, data) => callback(data))
  },
  onScanComplete: (callback: (data: unknown) => void): void => {
    ipcRenderer.on('scan-complete', (_event, data) => callback(data))
  },
  onFileFound: (callback: (data: unknown) => void): void => {
    ipcRenderer.on('file-found', (_event, data) => callback(data))
  }
}

contextBridge.exposeInMainWorld('api', api)
