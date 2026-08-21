import { app, BrowserWindow, dialog, powerMonitor } from 'electron'
import { join } from 'path'
import { registerIpcHandlers, broadcastScanComplete } from './ipc-handlers'
import { getEngine } from './native-bridge'
import {
  appendSessionLog,
  initSessionLog,
  isScanLive,
  setScanLive,
} from './session-log'

let mainWindow: BrowserWindow | null = null
let allowClose = false

app.commandLine.appendSwitch('disable-renderer-backgrounding')
app.commandLine.appendSwitch('disable-backgrounding-occluded-windows')

function createWindow(): void {
  mainWindow = new BrowserWindow({
    width: 1280,
    height: 800,
    minWidth: 1024,
    minHeight: 680,
    title: 'Byteback',
    backgroundColor: '#0D1117',
    show: true,
    webPreferences: {
      preload: join(__dirname, '../preload/index.js'),
      contextIsolation: true,
      nodeIntegration: false,
      sandbox: true
    }
  })

  mainWindow.on('ready-to-show', () => {
    mainWindow?.show()
  })

  const isDev = !!process.env.ELECTRON_RENDERER_URL
  if (isDev) {
    mainWindow.webContents.openDevTools()
  }

  if (process.env.ELECTRON_RENDERER_URL) {
    mainWindow.loadURL(process.env.ELECTRON_RENDERER_URL)
  } else {
    mainWindow.loadFile(join(__dirname, '../renderer/index.html'))
  }

  mainWindow.webContents.setWindowOpenHandler(() => ({ action: 'deny' }))
  mainWindow.webContents.on('will-navigate', (event, url) => {
    const current = mainWindow?.webContents.getURL() ?? ''
    if (url !== current) event.preventDefault()
  })
  mainWindow.webContents.on('render-process-gone', (_event, details) => {
    appendSessionLog('RENDER_GONE', `reason=${details.reason} exit=${details.exitCode}`)
    appendSessionLog('CRASH', `render ${details.reason}`)
  })

  mainWindow.on('close', (event) => {
    appendSessionLog('WINDOW_CLOSE', `scan_live=${isScanLive() ? 1 : 0}`)
    if (allowClose || !isScanLive() || !mainWindow) return
    event.preventDefault()
    void dialog.showMessageBox(mainWindow, {
      type: 'warning',
      buttons: ['Taramayı durdur ve çık', 'İptal'],
      defaultId: 1,
      cancelId: 1,
      message: 'Tarama sürüyor',
      detail: 'Pencereyi kapatmak taramayı öldürür. Kaldığın yer SQLite\'ta kalır; sonraki açılışta Devam et.',
    }).then((r) => {
      if (r.response !== 0) return
      try {
        getEngine().stopScan()
      } catch (e) {
        appendSessionLog('SCAN_STOP', `stop_failed ${e instanceof Error ? e.message : String(e)}`)
      }
      appendSessionLog('SCAN_STOP', 'window_close')
      setScanLive(false)
      allowClose = true
      mainWindow?.close()
    })
  })
}

app.whenReady().then(() => {
  initSessionLog(app.getPath('userData'))
  registerIpcHandlers()
  createWindow()

  powerMonitor.on('suspend', () => {
    appendSessionLog('OS_SLEEP', `scan_live=${isScanLive() ? 1 : 0}`)
    if (!isScanLive()) return
    try {
      getEngine().stopScan()
      appendSessionLog('SCAN_STOP', 'os_sleep_request')
    } catch (e) {
      appendSessionLog('SCAN_FAIL', `os_sleep_stop ${e instanceof Error ? e.message : String(e)}`)
    }
    setTimeout(() => {
      if (!isScanLive()) return
      try {
        const engine = getEngine()
        if (engine.isScanActive()) return
        const scanId = engine.getLatestUsableScanId()
        broadcastScanComplete(scanId > 0 ? scanId : -1, 4, 'os_sleep_fallback')
      } catch {
        broadcastScanComplete(-1, 4, 'os_sleep_fallback')
      }
    }, 6000)
  })
  powerMonitor.on('resume', () => {
    appendSessionLog('OS_WAKE')
    if (!isScanLive()) return
    try {
      const engine = getEngine()
      if (!engine.isScanActive()) {
        const scanId = engine.getLatestUsableScanId()
        broadcastScanComplete(scanId > 0 ? scanId : -1, 4, 'os_wake_sync')
      }
    } catch {
      /* native unavailable */
    }
  })
  powerMonitor.on('shutdown', () => appendSessionLog('OS_SHUTDOWN'))
  powerMonitor.on('lock-screen', () => appendSessionLog('OS_LOCK'))
})

function stopScanForSignal(sig: string): void {
  appendSessionLog('SIGNAL', sig)
  if (isScanLive()) {
    try {
      getEngine().stopScan()
    } catch {
      /* */
    }
    setScanLive(false)
  }
}

process.on('SIGINT', () => stopScanForSignal('SIGINT'))
process.on('SIGTERM', () => stopScanForSignal('SIGTERM'))

process.on('uncaughtException', (err) => {
  appendSessionLog('CRASH', `uncaughtException ${err.stack ?? err.message}`)
})
process.on('unhandledRejection', (reason) => {
  const text = reason instanceof Error ? (reason.stack ?? reason.message) : String(reason)
  appendSessionLog('CRASH', `unhandledRejection ${text}`)
})

app.on('child-process-gone', (_event, details) => {
  appendSessionLog('CRASH', `child-process-gone type=${details.type} reason=${details.reason} exit=${details.exitCode}`)
})

app.on('before-quit', () => {
  appendSessionLog('APP_QUIT', `scan_live=${isScanLive() ? 1 : 0}`)
  if (!isScanLive()) return
  try {
    getEngine().stopScan()
  } catch {
    /* quitting anyway */
  }
  setScanLive(false)
})

app.on('window-all-closed', () => {
  if (process.platform !== 'darwin') {
    app.quit()
  }
})

app.on('activate', () => {
  if (BrowserWindow.getAllWindows().length === 0) {
    createWindow()
  }
})
