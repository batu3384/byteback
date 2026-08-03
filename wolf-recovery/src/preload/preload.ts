import { contextBridge, ipcRenderer } from 'electron'

contextBridge.exposeInMainWorld('api', {
  getAppVersion: (): Promise<string> => ipcRenderer.invoke('get-app-version'),
  getEngineVersion: (): Promise<string> => ipcRenderer.invoke('get-engine-version'),
  isAdministrator: (): Promise<boolean> => ipcRenderer.invoke('is-administrator'),
  listDrives: (): Promise<unknown[]> => ipcRenderer.invoke('list-drives'),

  startScan: (driveIndex: number, scanType: string): void =>
    ipcRenderer.send('start-scan', { driveIndex, scanType }),
  stopScan: (): void => ipcRenderer.send('stop-scan'),

  onScanProgress: (callback: (data: unknown) => void): void => {
    ipcRenderer.on('scan-progress', (_event, data) => callback(data))
  },
  onFileFound: (callback: (data: unknown) => void): void => {
    ipcRenderer.on('file-found', (_event, data) => callback(data))
  },
  onScanComplete: (callback: (data: unknown) => void): void => {
    ipcRenderer.on('scan-complete', (_event, data) => callback(data))
  }
})
