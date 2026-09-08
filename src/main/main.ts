import { app, BrowserWindow, dialog, powerMonitor, session } from 'electron'
import { join } from 'path'
import { broadcastScanComplete, isImagingLive, registerIpcHandlers } from './ipc-handlers'
import { getEngine } from './native-bridge'
import { redactPaths } from './redact-paths'
import {
  appendSessionLog,
  initSessionLog,
  isScanLive,
  setScanLive,
} from './session-log'

let mainWindow: BrowserWindow | null = null
let allowClose = false

// e2e isolation: each test run gets a fresh userData (DB, localStorage),
// so tests cannot leak theme/language/scan state into each other. Dev/unpackaged
// only — a packaged forensic build must never relocate its evidence store (F4).
if (process.env.BYTEBACK_USER_DATA && !app.isPackaged) {
  app.setPath('userData', process.env.BYTEBACK_USER_DATA)
}

// Forensic exclusivity: two instances would fight over the same SQLite store and
// PhysicalDrive handles. The loser quits; the winner focuses its window.
const gotSingleInstanceLock = app.requestSingleInstanceLock()
if (!gotSingleInstanceLock) {
  app.quit()
} else {
  app.on('second-instance', () => {
    if (mainWindow) {
      if (mainWindow.isMinimized()) mainWindow.restore()
      mainWindow.focus()
    }
  })
}

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
  // DevTools yalnız elle açılır (F12 / Ctrl+Shift+I) — her açılışta otomatik
  // açılması geliştirme modunda bile kafa karıştırıcıydı.
  if (isDev && process.env.BYTEBACK_DEVTOOLS === '1') {
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
    appendSessionLog('WINDOW_CLOSE', `scan_live=${isScanLive() ? 1 : 0} imaging_live=${isImagingLive() ? 1 : 0}`)
    if (allowClose || (!isScanLive() && !isImagingLive()) || !mainWindow) return
    event.preventDefault()
    void dialog.showMessageBox(mainWindow, {
      type: 'warning',
      buttons: ['İşlemi durdur ve çık', 'İptal'],
      defaultId: 1,
      cancelId: 1,
      message: 'İşlem sürüyor',
      detail: 'Pencereyi kapatmak taramayı/imajlamayı öldürür. Tarama SQLite\'ta kaldığı yerden sürer; imaj yarım kalır.',
    }).then((r) => {
      if (r.response !== 0) return
      try {
        getEngine().stopScan()
      } catch (e) {
        appendSessionLog('SCAN_STOP', `stop_failed ${e instanceof Error ? e.message : String(e)}`)
      }
      try {
        getEngine().stopImaging()
      } catch (e) {
        appendSessionLog('SCAN_STOP', `imaging_stop_failed ${e instanceof Error ? e.message : String(e)}`)
      }
      appendSessionLog('SCAN_STOP', 'window_close')
      setScanLive(false)
      allowClose = true
      mainWindow?.close()
    })
  })
}

app.whenReady().then(() => {
  // Trust boundary: every custom session in this app resolves to
  // session.defaultSession (the offscreen PDF window sets no session), so one
  // handler covers all windows. Clipboard writes stay allowed (report/CSV
  // copy); media, notifications, geolocation and everything else is denied.
  session.defaultSession.setPermissionRequestHandler((_webContents, permission, callback) => {
    callback(permission === 'clipboard-sanitized-write')
  })
  // Sync companion of the request handler: Chromium's permission pre-checks
  // (navigator.permissions.query etc.) consult this handler and would
  // default-allow while the request handler denies — an inconsistent surface.
  session.defaultSession.setPermissionCheckHandler((_webContents, permission, _requestingOrigin) => {
    return permission === 'clipboard-sanitized-write'
  })

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
  if (isScanLive() || isImagingLive()) {
    try {
      getEngine().stopScan()
    } catch {
      /* */
    }
    try {
      getEngine().stopImaging()
    } catch {
      /* */
    }
    setScanLive(false)
  }
}

process.on('SIGINT', () => stopScanForSignal('SIGINT'))
process.on('SIGTERM', () => stopScanForSignal('SIGTERM'))

/** Strip user-identifying absolute path prefixes (userData, cwd) from log text. */
function redacted(text: string): string {
  return redactPaths(text, { userData: app.getPath('userData'), cwd: process.cwd() })
}

process.on('uncaughtException', (err) => {
  appendSessionLog('CRASH', `uncaughtException ${redacted(err.stack ?? err.message)}`)
})
process.on('unhandledRejection', (reason) => {
  const text = reason instanceof Error ? (reason.stack ?? reason.message) : String(reason)
  appendSessionLog('CRASH', `unhandledRejection ${redacted(text)}`)
})

app.on('child-process-gone', (_event, details) => {
  appendSessionLog('CRASH', `child-process-gone type=${details.type} reason=${details.reason} exit=${details.exitCode}`)
})

app.on('before-quit', () => {
  appendSessionLog('APP_QUIT', `scan_live=${isScanLive() ? 1 : 0} imaging_live=${isImagingLive() ? 1 : 0}`)
  if (!isScanLive() && !isImagingLive()) return
  try {
    getEngine().stopScan()
  } catch {
    /* quitting anyway */
  }
  try {
    getEngine().stopImaging()
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
