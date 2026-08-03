import { ipcMain, app } from 'electron'
import { getEngine } from './native-bridge'

export function registerIpcHandlers(): void {
  ipcMain.handle('get-app-version', () => {
    return app.getVersion()
  })

  ipcMain.handle('get-engine-version', () => {
    return getEngine().getVersion()
  })

  ipcMain.handle('is-administrator', () => {
    return getEngine().isAdministrator()
  })

  ipcMain.handle('list-drives', () => {
    try {
      return getEngine().listDrives()
    } catch (error) {
      console.error('Failed to list drives:', error)
      return []
    }
  })

  ipcMain.on('start-scan', (_event, _payload) => {
    // Will be implemented in Phase 2 — scan pipeline
    console.log('Scan requested (not yet implemented)')
  })

  ipcMain.on('stop-scan', () => {
    console.log('Scan stop requested (not yet implemented)')
  })
}
