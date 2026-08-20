import { ipcMain, IpcMainEvent, app, BrowserWindow, dialog } from 'electron'
import { join } from 'path'
import { getEngine } from './native-bridge'
import { hexDataOrNull } from '../shared/hex-read'
import { parseRecoverIds, parseRecoverIdList } from '../shared/recover-ids'
import { loadAllowedImageDest, saveAllowedImageDest } from './image-dest-allowlist'

export function registerIpcHandlers(): void {
  const allowlistPath = join(app.getPath('userData'), 'allowed-image-dest.json')
  const allowedImageDest = loadAllowedImageDest(allowlistPath)

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
      return '0.1.0 (native error)'
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

  ipcMain.handle('list-partitions', (_event, driveIndex: number) => {
    try {
      const engine = getEngine()
      return engine.listPartitions(driveIndex)
    } catch (err) {
      console.error('[IPC] list-partitions error:', err)
      return []
    }
  })

  ipcMain.handle('resolve-volume', (_event, letter: string) => {
    try {
      const engine = getEngine()
      return engine.resolveVolume(letter)
    } catch (err) {
      console.error('[IPC] resolve-volume error:', err)
      return null
    }
  })

  ipcMain.handle('list-volume-letters', () => {
    try {
      const engine = getEngine()
      return engine.listVolumeLetters()
    } catch (err) {
      console.error('[IPC] list-volume-letters error:', err)
      return []
    }
  })

  ipcMain.handle('start-scan', async (event, driveIndex: number, scanType: string, scanOptions?: {
    partitionIndex?: number
    partitionStartSector?: number
    partitionSizeInSectors?: number
  }) => {
    try {
      const engine = getEngine()
      
      const callback = (data: any) => {
        if (data.type === 'progress') {
          event.sender.send('scan-progress', { current: data.current, total: data.total, badSectors: data.badSectors })
        } else if (data.type === 'file') {
          event.sender.send('scan-file-found', data)
        } else if (data.type === 'complete') {
          event.sender.send('scan-complete', { scanId: data.scanId, status: data.status })
        }
      }
      
      const drivePath = driveIndex === -1 ? 'raid' : String(driveIndex)
      console.log('[IPC] start-scan drive:', drivePath, 'type:', scanType, 'opts:', scanOptions ?? {})
      const opts = scanOptions && Object.keys(scanOptions).length > 0 ? scanOptions : undefined
      if (opts) {
        return (engine.startScan as (a: string, b: string, c: object, d: (data: unknown) => void) => number)(
          drivePath, scanType, opts, callback)
      }
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

  ipcMain.handle('get-timeline-events', (_event, scanId: number, offset: number, limit: number, filter?: string) => {
    try {
      const engine = getEngine()
      return engine.getTimelineEvents(scanId, offset ?? 0, limit ?? 200, filter ?? '')
    } catch (err) {
      console.error('[IPC] get-timeline-events error:', err)
      return { total: 0, events: [] }
    }
  })

  ipcMain.handle('export-report-pdf', async (_event, html: string) => {
    try {
      const focused = BrowserWindow.getFocusedWindow()
      const opts: Electron.SaveDialogOptions = {
        title: 'PDF Raporunu Kaydet',
        defaultPath: `wolf-recovery-rapor-${new Date().toISOString().slice(0, 10)}.pdf`,
        filters: [{ name: 'PDF', extensions: ['pdf'] }],
      }
      const target = focused
        ? await dialog.showSaveDialog(focused, opts)
        : await dialog.showSaveDialog(opts)
      if (target.canceled || !target.filePath) return { success: false, canceled: true }

      // Render the report HTML off-screen and print it to PDF through
      // Chromium's native PDF engine — no third-party dependency, and the
      // output matches what a browser would print.
      const win = new BrowserWindow({
        show: false,
        webPreferences: { offscreen: true, sandbox: true },
      })
      await win.loadURL('data:text/html;charset=utf-8,' + encodeURIComponent(html))
      const pdf = await win.webContents.printToPDF({
        printBackground: true,
        margins: { marginType: 'default' },
        pageSize: 'A4',
      })
      win.destroy()

      const fs = await import('node:fs/promises')
      await fs.writeFile(target.filePath, pdf)
      return { success: true, path: target.filePath }
    } catch (err) {
      console.error('[IPC] export-report-pdf error:', err)
      return { success: false, error: String(err) }
    }
  })

  ipcMain.handle('get-audit-log', (_event, maxLines?: number) => {
    try {
      const engine = getEngine()
      return engine.getAuditLog(maxLines ?? 200)
    } catch (err) {
      console.error('[IPC] get-audit-log error:', err)
      return []
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
      const bytes = hexDataOrNull(res)
      if (bytes) return bytes
      console.warn('[IPC] read-hex-data failed:', res.error)
    } catch (err) {
      console.error('[IPC] read-hex-data error:', err)
    }
    return null
  })

  ipcMain.on('start-imaging', (event: IpcMainEvent, driveIndex: number, destPath: string, format?: string) => {
    try {
      if (!destPath || !allowedImageDest.has(destPath)) {
        event.reply('imaging-progress', { current: 0, total: 0 })
        return
      }
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

  ipcMain.handle('get-latest-scan-id', () => {
    try {
      const engine = getEngine()
      return engine.getLatestScanId()
    } catch (err) {
      console.error('[IPC] get-latest-scan-id error:', err)
      return -1
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

  ipcMain.handle('search-files', (_event, scanId: number, query: string, offset: number, limit: number, useRegex?: boolean, category?: string) => {
    try {
      if (useRegex && typeof query === 'string' && query.length > 128) return []
      const engine = getEngine()
      return engine.searchFiles(scanId, query, offset ?? 0, limit ?? 100, !!useRegex, category ?? '')
    } catch (err) {
      console.error('[IPC] search-files error:', err)
      return []
    }
  })

  ipcMain.handle('search-file-content', (_event, scanId: number, query: string, offset: number, limit: number) => {
    try {
      const engine = getEngine()
      return engine.searchFileContent(scanId, query, offset ?? 0, limit ?? 100)
    } catch (err) {
      console.error('[IPC] search-file-content error:', err)
      return []
    }
  })

  ipcMain.handle('start-content-search', async (event, scanId: number, query: string) => {
    try {
      const engine = getEngine()
      const callback = (data: any) => {
        if (data.type === 'progress') {
          event.sender.send('content-search-progress', { current: data.current, total: data.total })
        } else if (data.type === 'match') {
          event.sender.send('content-search-match', data)
        } else if (data.type === 'complete') {
          event.sender.send('content-search-complete', { status: data.status })
        }
      }
      return engine.startContentSearch(scanId, query, callback)
    } catch (err) {
      console.error('[IPC] start-content-search error:', err)
      return false
    }
  })

  ipcMain.on('stop-content-search', () => {
    try {
      const engine = getEngine()
      engine.stopContentSearch()
    } catch (err) {
      console.error('[IPC] stop-content-search error:', err)
    }
  })

  ipcMain.handle('get-scan-summary', (_event, scanId: number) => {
    try {
      const engine = getEngine()
      return engine.getScanSummary(scanId)
    } catch (err) {
      console.error('[IPC] get-scan-summary error:', err)
      return null
    }
  })

  ipcMain.handle('pick-and-wipe-file', async () => {
    try {
      const focused = BrowserWindow.getFocusedWindow()
      const openOpts: Electron.OpenDialogOptions = {
        title: 'İmha edilecek dosyayı seçin',
        properties: ['openFile'],
      }
      const picked = focused
        ? await dialog.showOpenDialog(focused, openOpts)
        : await dialog.showOpenDialog(openOpts)
      if (picked.canceled || picked.filePaths.length === 0) return false

      const target = picked.filePaths[0]
      const win = focused ?? BrowserWindow.getAllWindows()[0]
      if (!win) return false
      const confirm = await dialog.showMessageBox(win, {
        type: 'warning',
        buttons: ['İptal', 'Dosyayı imha et'],
        defaultId: 0,
        cancelId: 0,
        title: 'Dosya imhası',
        message: 'Seçilen dosya geri alınamaz biçimde üzerine yazılacak.',
        detail: target,
      })
      if (confirm.response !== 1) return false
      return await getEngine().startWipe(target)
    } catch (err) {
      console.error('[IPC] pick-and-wipe-file error:', err)
      return false
    }
  })

  ipcMain.handle('pick-and-wipe-freespace', async () => {
    try {
      const focused = BrowserWindow.getFocusedWindow()
      const openOpts: Electron.OpenDialogOptions = {
        title: 'Boş alanı doldurulacak klasör (hedef birim)',
        properties: ['openDirectory'],
      }
      const picked = focused
        ? await dialog.showOpenDialog(focused, openOpts)
        : await dialog.showOpenDialog(openOpts)
      if (picked.canceled || picked.filePaths.length === 0) return false

      const target = picked.filePaths[0]
      const win = focused ?? BrowserWindow.getAllWindows()[0]
      if (!win) return false
      const confirm = await dialog.showMessageBox(win, {
        type: 'warning',
        buttons: ['İptal', 'Boş alanı imha et'],
        defaultId: 0,
        cancelId: 0,
        title: 'Boş alan imhası',
        message: 'Seçilen birimin boş kümeleri geçici dosyayla doldurulup DoD 3 geçiş yazılır. Tahsisli dosyalar ve file slack dokunulmaz. Fiziksel disk yolu kabul edilmez.',
        detail: target,
      })
      if (confirm.response !== 1) return false
      return await getEngine().startWipe(target)
    } catch (err) {
      console.error('[IPC] pick-and-wipe-freespace error:', err)
      return false
    }
  })

  ipcMain.handle('set-bitlocker-fvek', (_event, hex: string) => {
    try {
      return getEngine().setBitLockerFvek(typeof hex === 'string' ? hex : '')
    } catch (err) {
      console.error('[IPC] set-bitlocker-fvek error:', err)
      return false
    }
  })

  ipcMain.handle('set-bitlocker-recovery-password', (_event, driveIndex: number, password: string) => {
    try {
      if (typeof driveIndex !== 'number' || typeof password !== 'string') {
        return 'invalid arguments'
      }
      return getEngine().setBitLockerRecoveryPassword(driveIndex, password)
    } catch (err) {
      console.error('[IPC] set-bitlocker-recovery-password error:', err)
      return 'native error'
    }
  })

  ipcMain.handle('set-bitlocker-password', (_event, driveIndex: number, password: string) => {
    try {
      if (typeof driveIndex !== 'number' || typeof password !== 'string') {
        return 'invalid arguments'
      }
      return getEngine().setBitLockerPassword(driveIndex, password)
    } catch (err) {
      console.error('[IPC] set-bitlocker-password error:', err)
      return 'native error'
    }
  })

  ipcMain.handle('wipe-physical-drive', async (_event, driveIndex: number, typedSerial: string, confirmPhrase?: string) => {
    try {
      if (typeof driveIndex !== 'number' || typeof typedSerial !== 'string' || !typedSerial.trim()) {
        return false
      }
      if (confirmPhrase !== 'IMHA') {
        return false
      }
      const engine = getEngine()
      const drives = engine.listDrives()
      const target = drives.find((d) => d.index === driveIndex)
      const win = BrowserWindow.getFocusedWindow() ?? BrowserWindow.getAllWindows()[0]
      if (!win) return false
      const confirm = await dialog.showMessageBox(win, {
        type: 'warning',
        buttons: ['İptal', 'Diski imha et'],
        defaultId: 0,
        cancelId: 0,
        title: 'PhysicalDrive imhası',
        message: `PhysicalDrive${driveIndex} baştan sona DoD 3 geçiş yazılacak. Geri alınamaz.`,
        detail: `${target?.model ?? 'disk'} | seri ${target?.serial ?? '?'} | ${target?.type ?? 'Unknown'}. SSD’de NIST 800-88 sanitization değildir. Native katman yazdığınız seriyi liste serisiyle karşılaştırmadan yazmaz.`,
      })
      if (confirm.response !== 1) return false
      return await getEngine().startPhysicalWipe(driveIndex, typedSerial)
    } catch (err) {
      console.error('[IPC] wipe-physical-drive error:', err)
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

  ipcMain.handle('fail-raid-disk', (_event, diskIndex: number) => {
    try {
      return getEngine().failRaidDisk(diskIndex)
    } catch (err) {
      console.error('[IPC] fail-raid-disk error:', err)
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

  ipcMain.handle('pick-save-image', async (_event, format: 'raw' | 'ewf') => {
    try {
      const focused = BrowserWindow.getFocusedWindow()
      const ext = format === 'ewf' ? 'E01' : 'dd'
      const opts: Electron.SaveDialogOptions = {
        title: 'Disk İmajını Kaydet',
        defaultPath: `wolf-image-${new Date().toISOString().slice(0, 10)}.${ext}`,
        filters: format === 'ewf'
          ? [{ name: 'EnCase EWF', extensions: ['E01', 'e01'] }]
          : [{ name: 'RAW Image', extensions: ['dd', 'img', 'raw'] }],
      }
      const target = focused
        ? await dialog.showSaveDialog(focused, opts)
        : await dialog.showSaveDialog(opts)
      if (target.canceled || !target.filePath) return null
      allowedImageDest.add(target.filePath)
      saveAllowedImageDest(allowlistPath, allowedImageDest)
      return target.filePath
    } catch (err) {
      console.error('[IPC] pick-save-image error:', err)
      return null
    }
  })

  ipcMain.handle('recover-file', async (_event, driveIndex: number, fileId: number, destDir: string, scanId: number) => {
    try {
      const parsed = parseRecoverIds(scanId, fileId)
      if (!parsed.ok) return { success: false, error: parsed.error }
      const engine = getEngine()
      return await engine.recoverFile(driveIndex, parsed.fileId, destDir, parsed.scanId)
    } catch (err) {
      console.error('[IPC] recover-file error:', err)
      return { success: false, error: String(err) }
    }
  })

  ipcMain.handle('recover-files-batch', async (_event, driveIndex: number, fileIds: number[], destDir: string, scanId: number) => {
    try {
      const parsed = parseRecoverIdList(scanId, fileIds)
      if (!parsed.ok) return { succeeded: 0, failed: fileIds?.length ?? 0, results: [], error: parsed.error }
      const engine = getEngine()
      return await engine.recoverFilesBatch(driveIndex, parsed.fileIds, destDir, parsed.scanId)
    } catch (err) {
      console.error('[IPC] recover-files-batch error:', err)
      return { succeeded: 0, failed: fileIds?.length ?? 0, results: [], error: String(err) }
    }
  })

  ipcMain.handle('read-file-preview', (_event, driveIndex: number, scanId: number, fileId: number) => {
    try {
      const parsed = parseRecoverIds(scanId, fileId)
      if (!parsed.ok) return { success: false, error: parsed.error }
      const engine = getEngine()
      return engine.readFilePreview(driveIndex, parsed.scanId, parsed.fileId)
    } catch (err) {
      console.error('[IPC] read-file-preview error:', err)
      return { success: false, error: String(err) }
    }
  })

  ipcMain.handle('get-raid-state', () => {
    try {
      return getEngine().getRaidState()
    } catch (err) {
      console.error('[IPC] get-raid-state error:', err)
      return { active: false, capacity: 0, numDisks: 0, level: -1 }
    }
  })

  ipcMain.handle('get-case-info', () => {
    try {
      return getEngine().getCaseInfo()
    } catch (err) {
      console.error('[IPC] get-case-info error:', err)
      return { caseNumber: '', investigator: '', agency: '', notes: '', createdAt: 0, updatedAt: 0 }
    }
  })

  ipcMain.handle('set-case-info', (_event, info: Record<string, string>) => {
    try {
      return getEngine().setCaseInfo(info)
    } catch (err) {
      console.error('[IPC] set-case-info error:', err)
      return false
    }
  })

  ipcMain.handle('lookup-nsrl', (_event, md5Hex: string) => {
    try {
      return getEngine().lookupNsrl(md5Hex)
    } catch (err) {
      console.error('[IPC] lookup-nsrl error:', err)
      return false
    }
  })

  ipcMain.handle('get-nsrl-stats', () => {
    try {
      return getEngine().getNsrlStats()
    } catch (err) {
      console.error('[IPC] get-nsrl-stats error:', err)
      return { count: 0, path: '' }
    }
  })

  ipcMain.handle('pick-and-load-nsrl', async () => {
    try {
      const focused = BrowserWindow.getFocusedWindow()
      const opts: Electron.OpenDialogOptions = {
        title: 'NSRL MD5 seti',
        properties: ['openFile'],
        filters: [
          { name: 'Hash list', extensions: ['txt', 'csv', 'hash'] },
          { name: 'All files', extensions: ['*'] },
        ],
      }
      const result = focused
        ? await dialog.showOpenDialog(focused, opts)
        : await dialog.showOpenDialog(opts)
      if (result.canceled || result.filePaths.length === 0) return null
      return getEngine().loadNsrl(result.filePaths[0])
    } catch (err) {
      console.error('[IPC] pick-and-load-nsrl error:', err)
      return { ok: false, count: 0, path: '' }
    }
  })
}

