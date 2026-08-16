import { ipcMain, IpcMainEvent, app, BrowserWindow, dialog } from 'electron'
import { join } from 'path'
import { getEngine } from './native-bridge'

export function registerIpcHandlers(): void {
  // Initialize SQLite database on startup
  try {
    const engine = getEngine()
    const dbPath = join(app.getPath('userData'), 'wolf_recovery.db')
    const ok = engine.initDatabase(dbPath)
    console.log('[IPC] Database initialized:', ok, 'at', dbPath)
  } catch (err) {
    console.error('[IPC] Database init failed:', err)
  }

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

  ipcMain.handle('start-scan', async (event, driveIndex: number, scanType: string) => {
    try {
      const engine = getEngine()
      
      const callback = (data: any) => {
        if (data.type === 'progress') {
          event.sender.send('scan-progress', { current: data.current, total: data.total })
        } else if (data.type === 'file') {
          event.sender.send('scan-file-found', { name: data.name, size: data.size })
        }
      }
      
      const drivePath = String(driveIndex)
      console.log('[IPC] start-scan drive:', drivePath, 'type:', scanType)
      return engine.startScan(drivePath, scanType, callback)

    } catch (err) {
      console.error('[IPC] start-scan error:', err)
      return -1
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

  ipcMain.on('start-imaging', (event: IpcMainEvent, driveIndex: number, destPath: string, format?: string) => {
    try {
      const engine = getEngine()

      const callback = (data: any) => {
        if (data.type === 'progress') {
          event.reply('imaging-progress', {
            current: data.current,
            total: data.total,
            md5: data.md5 ?? undefined,
          })
        }
      }

      console.log('[IPC] start-imaging drive:', driveIndex, 'dest:', destPath, 'format:', format ?? 'raw')
      engine.startImaging(driveIndex, destPath, callback, format === 'ewf' ? 'ewf' : 'raw')

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

  ipcMain.handle('start-wipe', async (_event, targetPath: string) => {
    try {
      const engine = getEngine()
      return await engine.startWipe(targetPath)
    } catch (err) {
      console.error('[IPC] start-wipe error:', err)
      return false
    }
  })

  ipcMain.handle('reconstruct-raid', (_event, driveIndices: number[], raidLevel: number) => {
    try {
      const engine = getEngine()
      console.log('[IPC] reconstruct-raid drives:', driveIndices, 'level:', raidLevel)
      return engine.reconstructRaid(driveIndices ?? [], raidLevel ?? 0)
    } catch (err) {
      console.error('[IPC] reconstruct-raid error:', err)
      return false
    }
  })

  ipcMain.handle('pick-directory', async () => {
    try {
      const focused = BrowserWindow.getFocusedWindow()
      const opts: Electron.OpenDialogOptions = {
        title: 'Hedef Klasör Seçin',
        properties: ['openDirectory', 'createDirectory'],
      }
      // Bind to the focused window when available so the dialog is modal to it;
      // otherwise fall back to the windowless overload.
      const result = focused
        ? await dialog.showOpenDialog(focused, opts)
        : await dialog.showOpenDialog(opts)
      if (result.canceled || result.filePaths.length === 0) return null
      return result.filePaths[0]
    } catch (err) {
      console.error('[IPC] pick-directory error:', err)
      return null
    }
  })

  ipcMain.handle('recover-file', async (_event, driveIndex: number, fileRecord: any, destDir: string) => {
    try {
      const engine = getEngine()
      console.log('[IPC] recover-file drive:', driveIndex, 'file:', fileRecord.name, 'dest:', destDir)
      return await engine.recoverFile(driveIndex, fileRecord, destDir)
    } catch (err) {
      console.error('[IPC] recover-file error:', err)
      return { success: false, error: String(err) }
    }
  })
}

