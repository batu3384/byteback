import { app, BrowserWindow } from 'electron'
import { appendFileSync } from 'fs'
import { join } from 'path'
import { registerIpcHandlers } from './ipc-handlers'

let mainWindow: BrowserWindow | null = null

function logCrash(line: string): void {
  const msg = `[crash] ${line}`
  console.error(msg)
  try {
    if (!app.isReady()) return
    appendFileSync(join(app.getPath('userData'), 'crash.log'), `${new Date().toISOString()} ${line}\n`)
  } catch {
    /* ignore secondary log failure */
  }
}

process.on('uncaughtException', (err) => {
  logCrash(`uncaughtException ${err.stack ?? err.message}`)
})
process.on('unhandledRejection', (reason) => {
  const text = reason instanceof Error ? (reason.stack ?? reason.message) : String(reason)
  logCrash(`unhandledRejection ${text}`)
})

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

  // Only open DevTools during development. In production builds the DevTools
  // would otherwise pop open on every launch, which is not acceptable for a
  // shipped forensic tool.
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
    logCrash(`render-process-gone reason=${details.reason} exit=${details.exitCode}`)
  })
}

app.whenReady().then(() => {
  registerIpcHandlers()
  createWindow()
})

app.on('child-process-gone', (_event, details) => {
  logCrash(`child-process-gone type=${details.type} reason=${details.reason} exit=${details.exitCode}`)
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
