import { ipcMain, IpcMainEvent } from 'electron'
import { getEngine } from './native-bridge'

export function registerIpcHandlers(): void {
  ipcMain.handle('get-version', () => {
    try {
      const engine = getEngine()
      return engine.getVersion()
    } catch (err) {
      return '1.0.0 (Mock)'
    }
  })

  ipcMain.handle('is-admin', () => {
    try {
      const engine = getEngine()
      return engine.isAdministrator()
    } catch (err) {
      return true // Mock fallback for UI
    }
  })

  ipcMain.handle('list-drives', () => {
    try {
      const engine = getEngine()
      return engine.listDrives()
    } catch (err) {
      // Mock data for UI testing
      return [
        { index: 0, model: 'Samsung SSD 980 PRO', serial: 'S5GXNF0R', sizeBytes: 1000204886016, sectorSize: 512, type: 'NVMe' },
        { index: 1, model: 'WD Blue SN570', serial: '22114780', sizeBytes: 500107862016, sectorSize: 512, type: 'NVMe' }
      ]
    }
  })

  // Start Scan handler with event streaming
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
      
      // In native engine, startScan takes path (e.g. \\.\PhysicalDrive0)
      const drivePath = `\\\\.\\PhysicalDrive${driveIndex}`
      engine.startScan(drivePath, scanType, callback)

    } catch (err) {
      // Mock Scan Behavior
      console.log('Native engine not available, running MOCK scan for UI testing.')
      let current = 0
      const total = 10000
      
      const timer = setInterval(() => {
        current += 100
        event.reply('scan-progress', { current, total })
        
        if (Math.random() > 0.5) {
          event.reply('scan-file-found', { 
            name: `mock_recovered_file_${current}.jpg`, 
            size: Math.floor(Math.random() * 5000000) 
          })
        }
        
        if (current >= total) {
          clearInterval(timer)
        }
      }, 100)
      
      // Save timer to a global or something if we want to support stop-scan in mock, 
      // but for this phase we'll just let it run out.
    }
  })
  
  ipcMain.on('stop-scan', () => {
    try {
      const engine = getEngine()
      engine.stopScan()
    } catch (err) {
      // Mock stop
      console.log('Mock scan stopped')
    }
  })
}
