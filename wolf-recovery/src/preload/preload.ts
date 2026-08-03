import { contextBridge, ipcRenderer } from 'electron'

contextBridge.exposeInMainWorld('api', {
  getAppVersion: (): Promise<string> => ipcRenderer.invoke('get-app-version'),
  getEngineVersion: (): Promise<string> => ipcRenderer.invoke('get-engine-version'),
  isAdministrator: (): Promise<boolean> => ipcRenderer.invoke('is-administrator'),
  listDrives: (): Promise<unknown[]> => ipcRenderer.invoke('list-drives'),

  startScan: (driveIndex: number, scanType: string): void =>
    ipcRenderer.send('start-scan', { driveIndex, scanType }),
  stopScan: (): void => ipcRenderer.send('stop-scan'),

  onScanProgress: (callback: (data: unknown) => void): (() => void) => {
    const listener = (_event: any, data: unknown) => callback(data)
    ipcRenderer.on('scan-progress', listener)
    return () => ipcRenderer.removeListener('scan-progress', listener)
  },
  onFileFound: (callback: (data: unknown) => void): (() => void) => {
    const listener = (_event: any, data: unknown) => callback(data)
    ipcRenderer.on('file-found', listener)
    return () => ipcRenderer.removeListener('file-found', listener)
  },
  onScanComplete: (callback: (data: unknown) => void): (() => void) => {
    const listener = (_event: any, data: unknown) => callback(data)
    ipcRenderer.on('scan-complete', listener)
    return () => ipcRenderer.removeListener('scan-complete', listener)
  }
})
