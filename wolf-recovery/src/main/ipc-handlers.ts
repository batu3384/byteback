import { ipcMain, IpcMainEvent } from 'electron'
import { getEngine } from './native-bridge'

export function registerIpcHandlers(): void {
  ipcMain.handle('get-version', () => {
    try {
      const engine = getEngine()
      return engine.getVersion()
    } catch (err) {
      console.error('[IPC] get-version error:', err)
      return '1.0.0 (Native Error)'
    }
  })

  ipcMain.handle('is-admin', () => {
    try {
      const engine = getEngine()
      return engine.isAdministrator()
    } catch (err) {
      console.error('[IPC] is-admin error:', err)
      return false
    }
  })

  ipcMain.handle('list-drives', () => {
    try {
      const engine = getEngine()
      const drives = engine.listDrives()
      console.log('[IPC] list-drives found:', drives.length, 'drives')
      return drives
    } catch (err) {
      console.error('[IPC] list-drives error:', err)
      return []
    }
  })

  ipcMain.on('start-scan', (event: IpcMainEvent, driveIndex: number, scanType: string) => {
    try {
      const engine = getEngine()
      
      const callback = (data: any) => {
        if (data.type === 'progress') {
          event.reply('scan-progress', { current: data.current, total: data.total })
        } else if (data.type === 'file') {
          event.reply('scan-file-found', { name: data.name, size: data.size })
        }
      }
      
      const drivePath = String(driveIndex)
      console.log('[IPC] start-scan drive:', drivePath, 'type:', scanType)
      engine.startScan(drivePath, scanType, callback)

    } catch (err) {
      console.error('[IPC] start-scan error:', err)
    }
  })
  
  ipcMain.on('stop-scan', () => {
    try {
      const engine = getEngine()
      engine.stopScan()
      console.log('[IPC] stop-scan')
    } catch (err) {
      console.error('[IPC] stop-scan error:', err)
    }
  })

  ipcMain.handle('get-smart-status', (_event, driveIndex) => {
    try {
      const engine = getEngine()
      return engine.getSmartStatus(driveIndex)
    } catch (err) {
      console.error('[IPC] get-smart-status error:', err)
      return { isValid: false }
    }
  })

  ipcMain.handle('read-hex-data', (_event, driveIndex, offset, size) => {
    try {
      const engine = getEngine()
      const res = engine.readSectors(driveIndex, offset, size)
      if (res.success && res.data) {
        return Array.from(res.data)
      }
      console.warn('[IPC] read-hex-data failed:', res.error)
    } catch (err) {
      console.error('[IPC] read-hex-data error:', err)
    }
    return []
  })

  ipcMain.on('start-imaging', (event: IpcMainEvent, driveIndex: number, destPath: string) => {
    try {
      const engine = getEngine()
      
      const callback = (data: any) => {
        if (data.type === 'progress') {
          event.reply('imaging-progress', { current: data.current, total: data.total })
        }
      }
      
      console.log('[IPC] start-imaging drive:', driveIndex, 'dest:', destPath)
      engine.startImaging(driveIndex, destPath, callback)

    } catch (err) {
      console.error('[IPC] start-imaging error:', err)
    }
  })
  
  ipcMain.on('stop-imaging', () => {
    try {
      const engine = getEngine()
      engine.stopImaging()
      console.log('[IPC] stop-imaging')
    } catch (err) {
      console.error('[IPC] stop-imaging error:', err)
    }
  })

  ipcMain.handle('get-file-count', (_event, scanId: number) => {
    try {
      const engine = getEngine()
      return engine.getFileCount(scanId)
    } catch (err) {
      console.error('[IPC] get-file-count error:', err)
      return 0
    }
  })

  ipcMain.handle('get-files-page', (_event, scanId: number, offset: number, limit: number) => {
    try {
      const engine = getEngine()
      return engine.getFilesPage(scanId, offset, limit)
    } catch (err) {
      console.error('[IPC] get-files-page error:', err)
      return []
    }
  })

  ipcMain.handle('get-scan-state', (_event, scanId: number) => {
    try {
      const engine = getEngine()
      return engine.getScanState(scanId)
    } catch (err) {
      console.error('[IPC] get-scan-state error:', err)
      return null
    }
  })
}

