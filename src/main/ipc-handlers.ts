import { existsSync, readdirSync, statSync, readFileSync, writeFileSync, mkdirSync } from 'fs'
import { ipcMain, IpcMainEvent, app, BrowserWindow, dialog } from 'electron'
import { basename, extname, join } from 'path'
import { getEngine } from './native-bridge'
import { hexDataOrNull, isHexDriveIndex, HEX_SEARCH_MAX_NEEDLE, HEX_SEARCH_MAX_HITS, SCAN_IMAGE_DRIVE_INDEX } from '../shared/hex-read'
import { HEX_MARKS_FILE, parseHexMarks, serializeHexMarks } from '../shared/hex-marks'
import { diskBusyMessage } from '../shared/scan-required'
import { isWin32VolumeDevicePath, isEvidenceImagePath } from '../shared/win32-volume-path'
import { raid5AlgorithmForReconstruct, raidOffsetSectorsForReconstruct, raidStripeForReconstruct } from '../shared/raid-geometry'
import { loadAllowedImageDest, saveAllowedImageDest } from './image-dest-allowlist'
import { findThumbPath, mimeForExt, storeThumb, thumbUrlFor, THUMB_DIR_NAME } from './thumb-cache'
import { callNative } from './ipc-native'
import { appendProgressLog, appendSessionLog, readSessionLog, setScanLive, isScanLive } from './session-log'
import { CRASH_DUMPS_DIR_NAME, latestCrashDump } from './crash-dumps'
import { assertDbReady, dbInitErrorMessage, getDbStatusPayload, isDbReady, setDbInitResult } from './ipc-db-state'
import { registerScanDbIpcHandlers } from './ipc-handlers-scan-db'
import { registerRecoverIpcHandlers } from './ipc-handlers-recover'
import { destOnEvidenceError, lastEvidenceDisksIncludes, setLastEvidenceDisks } from './ipc-evidence-context'
import { assertBoundDriveIndex, assertDriveIndex } from './ipc-guards'
import { clampInt, sanitizeCsvFileName, scanProgressPayload, sanitizeRaidImagePaths, sanitizeRaidIndices, startScanDrivePath } from './ipc-utils'
import { wipeSerialMatches } from './wipe-serial'

export {
  clampInt,
  sanitizeCsvFileName,
  scanProgressPayload,
  sanitizeRaidIndices,
  sanitizeRaidImagePaths,
  startScanDrivePath,
} from './ipc-utils'

export { auditChainEvent, asciiForAudit } from './ipc-audit-bridge'

let activeScanToken = 0
let imagingLive = false

function hexBindVolumePath(volumePath?: string): string | undefined {
  if (typeof volumePath !== 'string') return undefined
  if (isWin32VolumeDevicePath(volumePath) || isEvidenceImagePath(volumePath)) return volumePath
  return undefined
}

/** True while a native imaging run is in flight (window close / quit must stop it). */
export function isImagingLive(): boolean {
  return imagingLive
}

/** Notify renderer and clear main scan-live when native complete IPC may not arrive. */
export function broadcastScanComplete(scanId: number, status: number, reason: string): void {
  setScanLive(false)
  appendSessionLog('SCAN_STOP', reason)
  for (const w of BrowserWindow.getAllWindows()) {
    w.webContents.send('scan-complete', { scanId, status })
  }
}

export function registerIpcHandlers(): void {
  const allowlistPath = join(app.getPath('userData'), 'allowed-image-dest.json')
  const allowedImageDest = loadAllowedImageDest(allowlistPath)

  // Initialize SQLite database on startup
  try {
    const engine = getEngine()
    const dbPath = join(app.getPath('userData'), 'byteback.db')
    const ok = engine.initDatabase(dbPath)
    setDbInitResult(!!ok, ok ? null : 'initDatabase returned false')
    // Redact the absolute path: session.log must not embed C:\Users\<name> on
    // every line — the file lives in app.getPath('userData'), which is where
    // support should look for byteback.db.
    console.log('[IPC] Database initialized:', ok, 'db:', basename(dbPath))
    appendSessionLog('DB_OPEN', `ok=${ok ? 1 : 0} db=${basename(dbPath)} (tam yol userData dizininde)`)
    if (ok) {
      try {
        const usableId = engine.getLatestUsableScanId()
        if (usableId > 0) {
          const st = engine.getScanState(usableId)
          appendSessionLog(
            'SCAN_STATUS',
            `scanId=${usableId} status=${st.status} ${st.scannedSectors}/${st.totalSectors} type=${st.scanType}`,
          )
          if (st.status === 4) {
            appendSessionLog('SCAN_ORPHAN', `scanId=${usableId} paused_on_startup`)
          }
        }
      } catch (e) {
        appendSessionLog('DB_OPEN', `latest_scan_failed ${e instanceof Error ? e.message : String(e)}`)
      }
    }
  } catch (err) {
    const msg = err instanceof Error ? err.message : String(err)
    setDbInitResult(false, msg)
    console.error('[IPC] Database init failed:', err)
    appendSessionLog('CRASH', `db_init ${msg}`)
  }

  registerScanDbIpcHandlers()
  registerRecoverIpcHandlers()

  ipcMain.handle('get-db-status', () => getDbStatusPayload())

  ipcMain.handle('get-version', () =>
    callNative('get-version', () => getEngine().getVersion())
  )
  ipcMain.handle('get-carve-signature-count', () =>
    callNative('get-carve-signature-count', () => getEngine().getCarveSignatureCount())
  )

  ipcMain.handle('is-admin', () =>
    callNative('is-admin', () => getEngine().isAdministrator())
  )

  ipcMain.handle('list-drives', () =>
    callNative('list-drives', () => {
      const drives = getEngine().listDrives()
      console.log('[IPC] list-drives found:', drives.length, 'drives')
      return drives
    })
  )

  ipcMain.handle('list-partitions', (_event, driveIndex: number) => {
    assertDriveIndex(driveIndex)
    return callNative('list-partitions', () => getEngine().listPartitions(driveIndex))
  })

  ipcMain.handle('resolve-volume', (_event, letter: string) =>
    callNative('resolve-volume', () => getEngine().resolveVolume(letter))
  )

  ipcMain.handle('list-volume-letters', () =>
    callNative('list-volume-letters', () => getEngine().listVolumeLetters())
  )

  ipcMain.handle('start-scan', async (event, driveIndex: number, scanType: string, scanOptions?: import('../shared/ipc-contract').ScanOptions) => {
    if (!isDbReady()) {
      throw new Error(dbInitErrorMessage() ?? 'Veritabanı kullanılamıyor — tarama başlatılamaz')
    }
    // Trust boundary: native std::stoi failure silently defaults to drive 0,
    // so garbage drivePath would scan the wrong disk without error.
    // -1 RAID, -2 local evidence image (imagePath required, never PhysicalDrive).
    if (typeof scanType !== 'string' || !scanType) {
      throw new Error('Geçersiz tarama tipi')
    }
    const imagePath = typeof scanOptions?.imagePath === 'string' && isEvidenceImagePath(scanOptions.imagePath)
      ? scanOptions.imagePath
      : undefined
    if (imagePath) {
      if (driveIndex !== SCAN_IMAGE_DRIVE_INDEX) {
        throw new Error('Geçersiz sürücü indeksi')
      }
      if (!existsSync(imagePath)) {
        throw new Error('Geçersiz imaj yolu')
      }
    } else if (driveIndex === SCAN_IMAGE_DRIVE_INDEX) {
      throw new Error('Geçersiz imaj yolu')
    } else {
      assertBoundDriveIndex(driveIndex)
    }
    try {
      const engine = getEngine()
      const token = ++activeScanToken

      // CA-016: the native engine assigns the scan id synchronously inside
      // startScan, before any thread-safe callback can fire — stamp it onto
      // progress events so the renderer can drop stale scans.
      let boundScanId = -1
      const callback = (data: any) => {
        if (token !== activeScanToken) return
        if (data.type === 'progress') {
          appendProgressLog(data.current, data.total, data.phase)
          event.sender.send('scan-progress', scanProgressPayload(boundScanId, data))
        } else if (data.type === 'complete') {
          setScanLive(false)
          const st = Number(data.status)
          appendSessionLog(
            st === 1 ? 'SCAN_COMPLETE' : st === 3 ? 'SCAN_FAIL' : 'SCAN_STOP',
            `scanId=${data.scanId} status=${st}`,
          )
          event.sender.send('scan-complete', { scanId: data.scanId, status: data.status })
        }
      }

      const drivePath = startScanDrivePath(driveIndex, imagePath)
      const evidence = Array.isArray(scanOptions?.evidenceDiskIndices)
        ? scanOptions.evidenceDiskIndices.filter((n) => Number.isInteger(n) && n >= 0)
        : []
      setLastEvidenceDisks(evidence.length > 0 ? evidence : (driveIndex >= 0 ? [driveIndex] : []))
      const optsIn: import('../shared/ipc-contract').ScanOptions = { ...(scanOptions ?? {}) }
      if (typeof optsIn.volumePath === 'string') {
        if (!isWin32VolumeDevicePath(optsIn.volumePath)) delete optsIn.volumePath
      }
      if (imagePath) optsIn.imagePath = imagePath
      else delete optsIn.imagePath
      console.log('[IPC] start-scan drive:', drivePath, 'type:', scanType, 'opts:', optsIn)
      const opts = Object.keys(optsIn).length > 0 ? optsIn : undefined
      const id = opts
        ? engine.startScan(drivePath, scanType, opts, callback)
        : engine.startScan(drivePath, scanType, {}, callback)
      boundScanId = id
      if (id > 0) {
        setScanLive(true)
        appendSessionLog('SCAN_START', `scanId=${id} drive=${drivePath} type=${scanType}`)
      } else {
        appendSessionLog('SCAN_FAIL', `scanId=${id} drive=${drivePath} type=${scanType}`)
      }
      return id

    } catch (err) {
      setScanLive(false)
      appendSessionLog('SCAN_FAIL', err instanceof Error ? err.message : String(err))
      console.error('[IPC] start-scan error:', err)
      throw err
    }
  })
  
  ipcMain.on('stop-scan', () => {
    try {
      getEngine().stopScan()
      appendSessionLog('SCAN_STOP', 'user_request')
      console.log('[IPC] stop-scan requested')
    } catch (err) {
      console.error('[IPC] stop-scan error:', err)
      appendSessionLog('SCAN_FAIL', `stop_scan ${err instanceof Error ? err.message : String(err)}`)
    }
  })

  // e2e-only: seed a completed scan fixture into the app DB. Gated on an
  // explicit test flag — a production renderer must never fabricate scans.
  ipcMain.handle('seed-scan-fixture', (_event, files: Array<Record<string, unknown>>, imagePath?: unknown) => {
    if (process.env.BYTEBACK_E2E !== '1') {
      throw new Error('seed-scan-fixture is only available in e2e runs (BYTEBACK_E2E=1)')
    }
    const img = typeof imagePath === 'string' && isEvidenceImagePath(imagePath) ? imagePath : undefined
    return callNative('seed-scan-fixture', () => getEngine().seedScanFixture(files ?? [], img))
  })

  ipcMain.handle('seed-image-dest', (_event, destPath: unknown) => {
    if (process.env.BYTEBACK_E2E !== '1') {
      throw new Error('seed-image-dest is only available in e2e runs (BYTEBACK_E2E=1)')
    }
    if (typeof destPath !== 'string' || destPath.length === 0 || destPath.startsWith('\\\\.\\')) return false
    allowedImageDest.add(destPath)
    saveAllowedImageDest(allowlistPath, allowedImageDest)
    return true
  })

  ipcMain.handle('get-timeline-events', (_event, scanId: number, offset: number, limit: number, filter?: string) =>
    callNative('get-timeline-events', () =>
      getEngine().getTimelineEvents(
        clampInt(scanId, 0, Number.MAX_SAFE_INTEGER, 0),
        clampInt(offset, 0, Number.MAX_SAFE_INTEGER, 0),
        clampInt(limit, 1, 1000, 200),
        filter ?? '',
      )
    )
  )

  ipcMain.handle('export-report-pdf', async (_event, html: string) => {
    try {
      const focused = BrowserWindow.getFocusedWindow()
      const opts: Electron.SaveDialogOptions = {
        title: 'PDF Raporunu Kaydet',
        defaultPath: `byteback-rapor-${new Date().toISOString().slice(0, 10)}.pdf`,
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
      // data: URLs hit Chromium's URL length cap (~2MB encoded) — large
      // reports fail to load. A temp file has no such limit; loadFile is the
      // same document for the print engine.
      const fs = await import('node:fs/promises')
      const tempHtml = join(app.getPath('temp'), `byteback-report-${Date.now()}-${Math.random().toString(36).slice(2)}.html`)
      try {
        await fs.writeFile(tempHtml, html, 'utf-8')
        await win.loadFile(tempHtml)
        // Electron 44 removed marginType: margins are physical inches now
        // (default margins = Chromium print defaults, same intent).
        const pdf = await win.webContents.printToPDF({
          printBackground: true,
          pageSize: 'A4',
        })
        await fs.writeFile(target.filePath, pdf)
        return { success: true, path: target.filePath }
      } finally {
        // Never leak a hidden window or the temp report when
        // writeFile/loadFile/printToPDF throws. Destroy first so Windows
        // releases its handle before the unlink.
        win.destroy()
        await fs.unlink(tempHtml).catch(() => {})
      }
    } catch (err) {
      console.error('[IPC] export-report-pdf error:', err)
      return { success: false, error: String(err) }
    }
  })

  ipcMain.handle('get-audit-log', (_event, maxLines?: number) =>
    callNative('get-audit-log', () =>
      getEngine().getAuditLog(maxLines === undefined ? undefined : clampInt(maxLines, 1, 5000, 500))
    )
  )

  // FAZ 1.3c: streaming CSV export. Dest safety mirrors the imaging allowlist
  // pattern — the ONLY path native ever receives comes from this save dialog;
  // the renderer sends filter/labels/none of a path.
  ipcMain.handle('export-csv', async (
    _event,
    scanId: number,
    filter?: import('../shared/ipc-contract').FileListFilter,
    header?: string[],
    labels?: import('../shared/ipc-contract').CsvExportLabels,
    suggestedName?: string,
  ) => {
    try {
      assertDbReady()
      const sid = clampInt(scanId, 1, Number.MAX_SAFE_INTEGER, 0)
      if (sid <= 0) {
        return { success: false, error: 'Geçersiz tarama kimliği' }
      }
      if (!Array.isArray(header) || header.length !== 10 || header.some((h) => typeof h !== 'string' || h.length > 128)) {
        return { success: false, error: 'Geçersiz CSV başlığı' }
      }
      const noFsDate = typeof labels?.noFsDate === 'string' && labels.noFsDate.length <= 128 ? labels.noFsDate : 'no FS date'
      const noDate = typeof labels?.noDate === 'string' && labels.noDate.length <= 128 ? labels.noDate : '—'

      const focused = BrowserWindow.getFocusedWindow()
      const opts: Electron.SaveDialogOptions = {
        title: 'CSV Dışa Aktar',
        defaultPath: sanitizeCsvFileName(suggestedName),
        filters: [{ name: 'CSV', extensions: ['csv'] }],
      }
      const target = focused
        ? await dialog.showSaveDialog(focused, opts)
        : await dialog.showSaveDialog(opts)
      if (target.canceled || !target.filePath) return { success: false, canceled: true }

      const res = await getEngine().exportCsv(sid, target.filePath, filter, header, noFsDate, noDate)
      appendSessionLog('CSV_EXPORT', `scanId=${sid} rows=${res?.rows ?? 0}`)
      return { success: true, path: target.filePath, rows: res?.rows ?? 0 }
    } catch (err) {
      console.error('[IPC] export-csv error:', err)
      return { success: false, error: err instanceof Error ? err.message : String(err) }
    }
  })

  ipcMain.handle('verify-audit-log', () =>
    callNative('verify-audit-log', () => getEngine().verifyAuditLog())
  )

  // Field-evidence collection (field-test protocol 0.2): the examiner copies
  // these paths into the case record. All names are derivable locally — no
  // native call needed (audit log lives next to the DB as <db>.audit.log).
  ipcMain.handle('get-data-paths', () => {
    const userData = app.getPath('userData')
    const dbPath = join(userData, 'byteback.db')
    const crashDir = join(userData, CRASH_DUMPS_DIR_NAME)
    let lastCrashDump: string | null = null
    try {
      if (existsSync(crashDir)) {
        const entries: { name: string; mtimeMs: number }[] = []
        for (const name of readdirSync(crashDir)) {
          try {
            const st = statSync(join(crashDir, name))
            if (st.isFile()) entries.push({ name, mtimeMs: st.mtimeMs })
          } catch { /* skip unread dump */ }
        }
        const latest = latestCrashDump(entries)
        if (latest) lastCrashDump = join(crashDir, latest.name)
      }
    } catch { /* crash dir unread */ }
    return {
      userData,
      dbPath,
      sessionLog: join(userData, 'session.log'),
      auditLog: dbPath + '.audit.log',
      crashDumps: crashDir,
      lastCrashDump,
    }
  })

  ipcMain.handle('get-session-log', (_event, maxLines?: number) => {
    const n = typeof maxLines === 'number' && maxLines > 0 ? Math.min(maxLines, 500) : 80
    return readSessionLog(n)
  })

  ipcMain.handle('log-renderer-error', (_event, message: unknown, componentStack?: unknown) => {
    const msg = typeof message === 'string' ? message.slice(0, 500) : 'unknown'
    const stack = typeof componentStack === 'string' ? componentStack.slice(0, 4000) : ''
    appendSessionLog('RENDER_ERROR', stack ? `${msg} | ${stack}` : msg)
  })

  ipcMain.handle('get-smart-status', (_event, driveIndex) => {
    assertDriveIndex(driveIndex)
    return callNative('get-smart-status', () => getEngine().getSmartStatus(driveIndex))
  })

  // Evidence-access audit granularity (deliberate): raw per-sector reads are
  // NOT audit-logged. The native AuditLogger's LogDiskRead / LogFileRecovered
  // hooks stay unwired dead code on purpose — a deep scan or hex session reads
  // millions of sectors and per-sector entries would flood the hash-chained
  // log into uselessness. Evidence access is recorded at operation
  // granularity instead: SCAN_START/SCAN_COMPLETE (bridge_scan.cpp), RECOVER /
  // PREVIEW (bridge_wipe.cpp and below), imaging events (bridge_imager.cpp).
  ipcMain.handle('read-hex-data', (_event, driveIndex: number, offset: number, size: number, volumePath?: string) => {
    try {
      assertDbReady()
      // Align with assertDriveIndex: a fractional index would truncate in
      // native and read sectors from the wrong disk.
      if (!isHexDriveIndex(driveIndex)) {
        return { data: null, error: 'Geçersiz sürücü indeksi' }
      }
      if (!Number.isFinite(offset) || offset < 0) {
        return { data: null, error: 'Geçersiz sektör ofseti' }
      }
      const maxBytes = 65536
      const reqSize = Number.isFinite(size) ? Math.floor(size) : 0
      if (reqSize <= 0 || reqSize > maxBytes) {
        return { data: null, error: `Okuma boyutu 1–${maxBytes} bayt arasında olmalı` }
      }
      const engine = getEngine()
      const vp = hexBindVolumePath(volumePath)
      const res = vp
        ? engine.readSectors(driveIndex, offset, reqSize, vp)
        : engine.readSectors(driveIndex, offset, reqSize)
      const bytes = hexDataOrNull(res)
      if (bytes) return { data: bytes }
      const raw = res.error || 'Sektör okunamadı'
      const msg = diskBusyMessage(raw) ?? raw
      console.warn('[IPC] read-hex-data failed:', msg)
      return { data: null, error: msg }
    } catch (err) {
      const msg = err instanceof Error ? err.message : String(err)
      console.error('[IPC] read-hex-data error:', err)
      return { data: null, error: msg }
    }
  })

  ipcMain.handle('search-hex', async (_event, driveIndex: number, needle: unknown, maxHits?: number, volumePath?: string) => {
    try {
      assertDbReady()
      if (!isHexDriveIndex(driveIndex)) {
        return { hits: [], unread: false, error: 'Geçersiz sürücü indeksi' }
      }
      if (!Array.isArray(needle) && !Buffer.isBuffer(needle)) {
        return { hits: [], unread: false, error: 'Geçersiz iğne' }
      }
      const raw = Buffer.isBuffer(needle) ? Array.from(needle) : needle
      if (raw.length < 1 || raw.length > HEX_SEARCH_MAX_NEEDLE) {
        return { hits: [], unread: false, error: 'İğne 1–64 bayt olmalı' }
      }
      const bytes: number[] = []
      for (const v of raw) {
        const n = Number(v)
        if (!Number.isFinite(n)) return { hits: [], unread: false, error: 'Geçersiz iğne' }
        bytes.push(n & 0xff)
      }
      const cap = clampInt(maxHits, 1, HEX_SEARCH_MAX_HITS, HEX_SEARCH_MAX_HITS)
      const engine = getEngine()
      const vp = hexBindVolumePath(volumePath)
      const res = await engine.searchHex(driveIndex, Buffer.from(bytes), cap, vp)
      const hits = Array.isArray(res?.hits)
        ? res.hits.filter((n) => typeof n === 'number' && Number.isFinite(n) && n >= 0)
        : []
      return { hits, unread: !!res?.unread, error: res?.error }
    } catch (err) {
      const raw = err instanceof Error ? err.message : String(err)
      const msg = diskBusyMessage(raw) ?? raw
      console.error('[IPC] search-hex error:', err)
      return { hits: [], unread: false, error: msg }
    }
  })

  ipcMain.handle('get-mft-record', async (_event, driveIndex: number, mftRef: unknown, volumePath?: string) => {
    try {
      assertDbReady()
      if (!isHexDriveIndex(driveIndex)) {
        return { ok: false, unread: false, error: 'Geçersiz sürücü indeksi' }
      }
      const ref = clampInt(mftRef, 0, 1_000_000_000, -1)
      if (ref < 0) return { ok: false, unread: false, error: 'Geçersiz MFT referansı' }
      const engine = getEngine()
      const vp = hexBindVolumePath(volumePath)
      const res = await engine.getMftRecord(driveIndex, ref, vp)
      return {
        ok: !!res?.ok,
        unread: !!res?.unread,
        mftRef: res?.mftRef,
        byteOffset: res?.byteOffset,
        signature: res?.signature,
        flags: res?.flags,
        attrs: Array.isArray(res?.attrs) ? res.attrs : [],
        error: res?.error,
      }
    } catch (err) {
      const raw = err instanceof Error ? err.message : String(err)
      const msg = diskBusyMessage(raw) ?? raw
      console.error('[IPC] get-mft-record error:', err)
      return { ok: false, unread: false, error: msg }
    }
  })

  ipcMain.handle('get-hex-marks', () => {
    const path = join(app.getPath('userData'), HEX_MARKS_FILE)
    try {
      return parseHexMarks(readFileSync(path, 'utf8'))
    } catch {
      return []
    }
  })

  ipcMain.handle('set-hex-marks', (_event, body: unknown) => {
    try {
      const marks = parseHexMarks(JSON.stringify({ marks: Array.isArray(body) ? body : [] }))
      const dir = app.getPath('userData')
      mkdirSync(dir, { recursive: true })
      writeFileSync(join(dir, HEX_MARKS_FILE), serializeHexMarks(marks), 'utf8')
      return { ok: true }
    } catch (err) {
      const msg = err instanceof Error ? err.message : String(err)
      console.error('[IPC] set-hex-marks error:', err)
      return { ok: false, error: msg }
    }
  })

  ipcMain.on('start-imaging', (event: IpcMainEvent, driveIndex: number, destPath: string, format?: string, volumePath?: string) => {
    try {
      // Native Int32Value(NaN) is 0 — garbage must not image the wrong disk.
      // -1 is the assembled RAID sentinel (same as hex/scan).
      if (!isHexDriveIndex(driveIndex)) {
        event.reply('imaging-progress', { current: 0, total: 0, error: 'Geçersiz sürücü indeksi' })
        return
      }
      if (!destPath || !allowedImageDest.has(destPath)) {
        event.reply('imaging-progress', { current: 0, total: 0, error: 'Destination not in allowlist' })
        return
      }
      const raidImaging = driveIndex === -1
      if (raidImaging) {
        const raid = getEngine().getRaidState()
        if (!raid.active) {
          event.reply('imaging-progress', { current: 0, total: 0, error: 'RAID dizisi birleştirilmedi' })
          return
        }
      }
      const vp = !raidImaging ? hexBindVolumePath(volumePath) : undefined
      if (driveIndex === SCAN_IMAGE_DRIVE_INDEX && !vp) {
        event.reply('imaging-progress', { current: 0, total: 0, error: 'Geçersiz imaj yolu' })
        return
      }
      const extraDisks: number[] = []
      if (vp && isWin32VolumeDevicePath(vp)) {
        try {
          const letter = vp.charAt(4)
          const rv = getEngine().resolveVolume(letter)
          if (rv) {
            if (Number.isInteger(rv.driveIndex) && rv.driveIndex >= 0) extraDisks.push(rv.driveIndex)
            if (Array.isArray(rv.diskNumbers)) extraDisks.push(...rv.diskNumbers)
          }
        } catch { /* dest-on-source still runs with driveIndex + lastEvidenceDisks */ }
      }
      const onSource = destOnEvidenceError(destPath, driveIndex, undefined, extraDisks)
      if (onSource) {
        appendSessionLog('IMAGE_FAIL', `drive=${driveIndex} dest_rejected: ${onSource}`)
        event.reply('imaging-progress', { current: 0, total: 0, error: onSource })
        return
      }
      const engine = getEngine()

      const callback = (data: any) => {
        if (data.type === 'progress') {
          // total===0 is the native failure tick (0,0); current>=total the
          // completion tick — both end the run, so window close can rely on it.
          if (data.total === 0 || data.current >= data.total) imagingLive = false
          event.reply('imaging-progress', {
            current: data.current,
            total: data.total,
            md5: data.md5 ?? undefined,
          })
        }
      }

      console.log('[IPC] start-imaging drive:', driveIndex, 'dest:', destPath, 'format:', format ?? 'raw', vp ? `volume: ${vp}` : '')
      const started = vp
        ? engine.startImaging(driveIndex, destPath, callback, format === 'ewf' ? 'ewf' : 'raw', vp)
        : engine.startImaging(driveIndex, destPath, callback, format === 'ewf' ? 'ewf' : 'raw')
      imagingLive = !!started
      if (!started) {
        // Renderer treats {total: 0, error} as failure — reply or it waits forever.
        event.reply('imaging-progress', { current: 0, total: 0, error: 'İmajlama başlatılamadı (disk meşgul olabilir)' })
      }

    } catch (err) {
      imagingLive = false
      console.error('[IPC] start-imaging error:', err)
      const msg = err instanceof Error ? err.message : String(err)
      event.reply('imaging-progress', { current: 0, total: 0, error: msg })
    }
  })

  ipcMain.on('stop-imaging', (event: IpcMainEvent) => {
    imagingLive = false
    try {
      const engine = getEngine()
      engine.stopImaging()
      console.log('[IPC] stop-imaging')
    } catch (err) {
      console.error('[IPC] stop-imaging error:', err)
    }
    // Explicit terminal marker: the native abort path never emits the
    // completion tick, so without this the renderer would infer "cancelled"
    // only from the absence of further events.
    event.reply('imaging-progress', { current: 0, total: 0, status: 'cancelled' })
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
      if (picked.canceled || picked.filePaths.length === 0) {
        return { ok: false, error: 'Dosya seçilmedi' }
      }

      const target = picked.filePaths[0]
      const onSource = destOnEvidenceError(target, -1)
      if (onSource) {
        appendSessionLog('WIPE_FAIL', `file dest_rejected: ${onSource}`)
        return { ok: false, error: onSource }
      }
      const win = focused ?? BrowserWindow.getAllWindows()[0]
      if (!win) return { ok: false, error: 'Onay penceresi açılamadı' }
      const confirm = await dialog.showMessageBox(win, {
        type: 'warning',
        buttons: ['İptal', 'Dosyayı imha et'],
        defaultId: 0,
        cancelId: 0,
        title: 'Dosya imhası',
        message: 'Seçilen dosya geri alınamaz biçimde üzerine yazılacak.',
        detail: target,
      })
      if (confirm.response !== 1) return { ok: false, error: 'İşlem iptal edildi' }
      const ok = await getEngine().startWipe(target)
      return { ok: !!ok, error: ok ? undefined : 'Dosya imhası başarısız (başka disk işlemi sürüyor olabilir)' }
    } catch (err) {
      const msg = err instanceof Error ? err.message : String(err)
      console.error('[IPC] pick-and-wipe-file error:', err)
      return { ok: false, error: msg }
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
      if (picked.canceled || picked.filePaths.length === 0) {
        return { ok: false, error: 'Klasör seçilmedi' }
      }

      const target = picked.filePaths[0]
      const onSource = destOnEvidenceError(target, -1)
      if (onSource) {
        appendSessionLog('WIPE_FAIL', `freespace dest_rejected: ${onSource}`)
        return { ok: false, error: onSource }
      }
      const win = focused ?? BrowserWindow.getAllWindows()[0]
      if (!win) return { ok: false, error: 'Onay penceresi açılamadı' }
      const confirm = await dialog.showMessageBox(win, {
        type: 'warning',
        buttons: ['İptal', 'Boş alanı imha et'],
        defaultId: 0,
        cancelId: 0,
        title: 'Boş alan imhası',
        message: 'Seçilen birimin boş kümeleri geçici dosyayla doldurulup DoD 3 geçiş yazılır. Tahsisli dosyalar ve file slack dokunulmaz. Fiziksel disk yolu kabul edilmez.',
        detail: target,
      })
      if (confirm.response !== 1) return { ok: false, error: 'İşlem iptal edildi' }
      const ok = await getEngine().startWipe(target)
      return { ok: !!ok, error: ok ? undefined : 'Boş alan imhası başarısız (başka disk işlemi sürüyor olabilir)' }
    } catch (err) {
      const msg = err instanceof Error ? err.message : String(err)
      console.error('[IPC] pick-and-wipe-freespace error:', err)
      return { ok: false, error: msg }
    }
  })

  ipcMain.handle('set-bitlocker-fvek', (_event, hex: string) => {
    try {
      return getEngine().setBitLockerFvek(typeof hex === 'string' ? hex : '')
    } catch {
      console.error('[IPC] set-bitlocker-fvek error')
      return false
    }
  })

  ipcMain.handle('set-bitlocker-recovery-password', (_event, driveIndex: number, password: string) => {
    try {
      if (typeof driveIndex !== 'number' || !Number.isInteger(driveIndex) || driveIndex < 0 || typeof password !== 'string') {
        return 'invalid arguments'
      }
      return getEngine().setBitLockerRecoveryPassword(driveIndex, password)
    } catch {
      console.error('[IPC] set-bitlocker-recovery-password error')
      return 'native error'
    }
  })

  ipcMain.handle('set-bitlocker-password', (_event, driveIndex: number, password: string) => {
    try {
      if (typeof driveIndex !== 'number' || !Number.isInteger(driveIndex) || driveIndex < 0 || typeof password !== 'string') {
        return 'invalid arguments'
      }
      return getEngine().setBitLockerPassword(driveIndex, password)
    } catch {
      console.error('[IPC] set-bitlocker-password error')
      return 'native error'
    }
  })

  ipcMain.handle('set-luks-password', (_event, driveIndex: number, password: string) => {
    try {
      if (typeof driveIndex !== 'number' || !Number.isInteger(driveIndex) || typeof password !== 'string') {
        return 'invalid arguments'
      }
      if (driveIndex < 0 && driveIndex !== SCAN_IMAGE_DRIVE_INDEX) return 'invalid arguments'
      return getEngine().setLuksPassword(driveIndex, password)
    } catch {
      console.error('[IPC] set-luks-password error')
      return 'native error'
    }
  })

  ipcMain.handle('wipe-physical-drive', async (_event, driveIndex: number, typedSerial: string, confirmPhrase?: string) => {
    try {
      // Destructive path: a fractional index would truncate in native and
      // wipe the wrong physical drive — integer >= 0 required.
      if (typeof driveIndex !== 'number' || !Number.isInteger(driveIndex) || driveIndex < 0 || typeof typedSerial !== 'string' || !typedSerial.trim()) {
        return { ok: false, error: 'Geçersiz sürücü veya seri numarası' }
      }
      if (confirmPhrase !== 'IMHA') {
        return { ok: false, error: 'Onay ifadesi IMHA olmalı' }
      }
      if (isScanLive() && lastEvidenceDisksIncludes(driveIndex)) {
        appendSessionLog('WIPE_FAIL', `physical dest_rejected drive=${driveIndex}`)
        return { ok: false, error: 'Tarama sürerken kanıt diski imha edilemez' }
      }
      if (imagingLive) {
        appendSessionLog('WIPE_FAIL', `physical dest_rejected imaging_live drive=${driveIndex}`)
        return { ok: false, error: 'İmaj sürerken disk imha edilemez' }
      }
      const engine = getEngine()
      const drives = engine.listDrives()
      const target = drives.find((d) => d.index === driveIndex)
      const win = BrowserWindow.getFocusedWindow() ?? BrowserWindow.getAllWindows()[0]
      if (!win) return { ok: false, error: 'Onay penceresi açılamadı' }
      const confirm = await dialog.showMessageBox(win, {
        type: 'warning',
        buttons: ['İptal', 'Diski imha et'],
        defaultId: 0,
        cancelId: 0,
        title: 'PhysicalDrive imhası',
        message: `PhysicalDrive${driveIndex} baştan sona DoD 3 geçiş yazılacak. Geri alınamaz.`,
        detail: `${target?.model ?? 'disk'} | seri ${target?.serial ?? '?'} | ${target?.type ?? 'Unknown'}. SSD’de NIST 800-88 sanitization değildir. Native katman yazdığınız seriyi liste serisiyle karşılaştırmadan yazmaz.`,
      })
      if (confirm.response !== 1) return { ok: false, error: 'İşlem iptal edildi' }
      const fresh = getEngine().listDrives().find((d) => d.index === driveIndex)
      if (!fresh?.serial || !wipeSerialMatches(typedSerial, fresh.serial)) {
        appendSessionLog('WIPE_FAIL', `physical serial_mismatch drive=${driveIndex}`)
        return { ok: false, error: 'Disk seri numarası değişti veya eşleşmiyor — imha iptal' }
      }
      if (isScanLive() && lastEvidenceDisksIncludes(driveIndex)) {
        appendSessionLog('WIPE_FAIL', `physical dest_rejected drive=${driveIndex}`)
        return { ok: false, error: 'Tarama sürerken kanıt diski imha edilemez' }
      }
      const ok = await getEngine().startPhysicalWipe(driveIndex, typedSerial)
      return { ok: !!ok, error: ok ? undefined : 'Disk imhası başarısız (seri eşleşmesi veya native hata)' }
    } catch (err) {
      const msg = err instanceof Error ? err.message : String(err)
      console.error('[IPC] wipe-physical-drive error:', err)
      return { ok: false, error: msg }
    }
  })

  ipcMain.handle('detect-raid', (_event, driveIndices: number[]) => {
    try {
      // Bounded, de-duplicated member set — see sanitizeRaidIndices.
      const indices = sanitizeRaidIndices(driveIndices, 64)
      if (indices.length < 2) {
        return { found: false }
      }
      const engine = getEngine()
      console.log('[IPC] detect-raid drives:', indices)
      return engine.detectRaid(indices)
    } catch (err) {
      console.error('[IPC] detect-raid error:', err)
      const msg = err instanceof Error ? err.message : String(err)
      return { found: false, error: msg }
    }
  })

  ipcMain.handle('detect-raid-images', (_event, imagePaths: unknown) => {
    try {
      const paths = sanitizeRaidImagePaths(imagePaths, 16)
      if (paths.length < 2) {
        return { found: false }
      }
      const engine = getEngine()
      return engine.detectRaidImages(paths)
    } catch (err) {
      console.error('[IPC] detect-raid-images error:', err)
      const msg = err instanceof Error ? err.message : String(err)
      return { found: false, error: msg }
    }
  })

  ipcMain.handle('reconstruct-raid', (_event, driveIndices: number[], raidLevel: number, blockSize: unknown, dataOffsetSectors?: unknown, raid5Algorithm?: unknown) => {
    try {
      // Same bounded/deduped member set as detect-raid: duplicate members would
      // assemble a bogus array, an unbounded array would freeze the main process.
      const indices = sanitizeRaidIndices(driveIndices, 64)
      if (indices.length < 1 || typeof raidLevel !== 'number' || !Number.isInteger(raidLevel)) {
        return { success: false, capacity: 0, numDisks: 0, error: 'Geçersiz RAID argümanları' }
      }
      const stripe = raidStripeForReconstruct(raidLevel, blockSize)
      const offset = raidOffsetSectorsForReconstruct(dataOffsetSectors)
      const algo = raid5AlgorithmForReconstruct(raid5Algorithm)
      if (stripe === null || offset === null || algo === null) {
        return { success: false, capacity: 0, numDisks: 0, error: 'Geçersiz RAID şerit/offset' }
      }
      const engine = getEngine()
      console.log('[IPC] reconstruct-raid drives:', indices, 'level:', raidLevel, 'stripe:', stripe, 'offsetSectors:', offset, 'raid5Algorithm:', algo)
      return engine.reconstructRaid(indices, raidLevel, stripe, offset, algo)
    } catch (err) {
      console.error('[IPC] reconstruct-raid error:', err)
      // Renderer expects RaidAssemblyResult, never a bare boolean.
      const msg = err instanceof Error ? err.message : String(err)
      return { success: false, capacity: 0, numDisks: 0, error: msg }
    }
  })

  ipcMain.handle('reconstruct-raid-images', (_event, imagePaths: unknown, raidLevel: number, blockSize: unknown, dataOffsetSectors?: unknown, raid5Algorithm?: unknown) => {
    try {
      const paths = sanitizeRaidImagePaths(imagePaths, 16)
      if (paths.length < 2 || typeof raidLevel !== 'number' || !Number.isInteger(raidLevel)) {
        return { success: false, capacity: 0, numDisks: 0, error: 'Geçersiz RAID argümanları' }
      }
      const stripe = raidStripeForReconstruct(raidLevel, blockSize)
      const offset = raidOffsetSectorsForReconstruct(dataOffsetSectors)
      const algo = raid5AlgorithmForReconstruct(raid5Algorithm)
      if (stripe === null || offset === null || algo === null) {
        return { success: false, capacity: 0, numDisks: 0, error: 'Geçersiz RAID şerit/offset' }
      }
      const engine = getEngine()
      return engine.reconstructRaidImages(paths, raidLevel, stripe, offset, algo)
    } catch (err) {
      console.error('[IPC] reconstruct-raid-images error:', err)
      const msg = err instanceof Error ? err.message : String(err)
      return { success: false, capacity: 0, numDisks: 0, error: msg }
    }
  })

  ipcMain.handle('assemble-lvm', (_event, driveIndices: unknown) => {
    try {
      const indices = sanitizeRaidIndices(driveIndices, 16)
      if (indices.length < 2) {
        return { success: false, capacity: 0, numDisks: 0, error: 'Geçersiz LVM PV listesi' }
      }
      return getEngine().assembleLvm(indices)
    } catch (err) {
      console.error('[IPC] assemble-lvm error:', err)
      const msg = err instanceof Error ? err.message : String(err)
      return { success: false, capacity: 0, numDisks: 0, error: msg }
    }
  })

  ipcMain.handle('assemble-lvm-images', (_event, imagePaths: unknown) => {
    try {
      const paths = sanitizeRaidImagePaths(imagePaths, 16)
      if (paths.length < 2) {
        return { success: false, capacity: 0, numDisks: 0, error: 'Geçersiz LVM PV imaj listesi' }
      }
      return getEngine().assembleLvmImages(paths)
    } catch (err) {
      console.error('[IPC] assemble-lvm-images error:', err)
      const msg = err instanceof Error ? err.message : String(err)
      return { success: false, capacity: 0, numDisks: 0, error: msg }
    }
  })

  ipcMain.handle('assemble-ldm', (_event, driveIndices: unknown) => {
    try {
      const indices = sanitizeRaidIndices(driveIndices, 16)
      if (indices.length < 2) {
        return { success: false, capacity: 0, numDisks: 0, error: 'Geçersiz LDM disk listesi' }
      }
      return getEngine().assembleLdm(indices)
    } catch (err) {
      console.error('[IPC] assemble-ldm error:', err)
      const msg = err instanceof Error ? err.message : String(err)
      return { success: false, capacity: 0, numDisks: 0, error: msg }
    }
  })

  ipcMain.handle('assemble-ldm-images', (_event, imagePaths: unknown) => {
    try {
      const paths = sanitizeRaidImagePaths(imagePaths, 16)
      if (paths.length < 2) {
        return { success: false, capacity: 0, numDisks: 0, error: 'Geçersiz LDM imaj listesi' }
      }
      return getEngine().assembleLdmImages(paths)
    } catch (err) {
      console.error('[IPC] assemble-ldm-images error:', err)
      const msg = err instanceof Error ? err.message : String(err)
      return { success: false, capacity: 0, numDisks: 0, error: msg }
    }
  })

  ipcMain.handle('fail-raid-disk', (_event, diskIndex: number) => {
    try {
      if (typeof diskIndex !== 'number' || !Number.isInteger(diskIndex) || diskIndex < 0) return false
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
        defaultPath: `byteback-image-${new Date().toISOString().slice(0, 10)}.${ext}`,
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

  // P0-2: TestDisk-style lost partition search (async, heavyOp-gated native).
  ipcMain.handle('scan-lost-partitions', (_event, driveIndex: number, stepSectors?: number) => {
    assertDriveIndex(driveIndex)
    return callNative('scan-lost-partitions', () =>
      getEngine().scanLostPartitions(driveIndex, stepSectors))
  })

  // P0-3: user signature overlay (resource-format JSON). Native returns false
  // if the file is missing or has no parseable signature objects.
  ipcMain.handle('pick-and-set-signature-overlay', async () => {
    try {
      const focused = BrowserWindow.getFocusedWindow()
      const opts: Electron.OpenDialogOptions = {
        title: 'İmza overlay JSON',
        properties: ['openFile'],
        filters: [
          { name: 'JSON', extensions: ['json'] },
          { name: 'All files', extensions: ['*'] },
        ],
      }
      const picked = focused
        ? await dialog.showOpenDialog(focused, opts)
        : await dialog.showOpenDialog(opts)
      if (picked.canceled || picked.filePaths.length === 0) return null
      const path = picked.filePaths[0]
      if (!path || !existsSync(path)) return { ok: false }
      const ok = (getEngine() as unknown as { setSignatureOverlay: (p: string) => boolean }).setSignatureOverlay(path)
      return { ok: !!ok }
    } catch (err) {
      console.error('[IPC] pick-and-set-signature-overlay error:', err)
      return { ok: false }
    }
  })

  ipcMain.handle('get-raid-state', () => callNative('get-raid-state', () => getEngine().getRaidState()))

  ipcMain.handle('get-case-info', () => callNative('get-case-info', () => getEngine().getCaseInfo()))

  ipcMain.handle('set-case-info', (_event, info: Record<string, string>) => {
    try {
      return getEngine().setCaseInfo(info)
    } catch (err) {
      console.error('[IPC] set-case-info error:', err)
      return false
    }
  })

  ipcMain.handle('lookup-nsrl', (_event, md5Hex: string) => callNative('lookup-nsrl', () => {
    assertDbReady()
    return getEngine().lookupNsrl(md5Hex)
  }))

  ipcMain.handle('get-nsrl-stats', () => callNative('get-nsrl-stats', () => getEngine().getNsrlStats()))

  ipcMain.handle('pick-scan-image', async () => {
    try {
      const focused = BrowserWindow.getFocusedWindow()
      const opts: Electron.OpenDialogOptions = {
        title: 'Kanıt imajı',
        properties: ['openFile'],
        filters: [
          { name: 'Disk images', extensions: ['dd', 'img', 'raw', 'e01', 'E01', 'ex01', 'vhd', 'vhdx', 'vmdk', 'vdi', 'qcow2', 'qcow', 'dmg'] },
          { name: 'All files', extensions: ['*'] },
        ],
      }
      const result = focused
        ? await dialog.showOpenDialog(focused, opts)
        : await dialog.showOpenDialog(opts)
      if (result.canceled || result.filePaths.length === 0) return null
      const picked = result.filePaths[0]
      return isEvidenceImagePath(picked) ? picked : null
    } catch (err) {
      console.error('[IPC] pick-scan-image error:', err)
      return null
    }
  })

  ipcMain.handle('pick-raid-member-images', async () => {
    try {
      const focused = BrowserWindow.getFocusedWindow()
      const opts: Electron.OpenDialogOptions = {
        title: 'RAID üye imajları',
        properties: ['openFile', 'multiSelections'],
        filters: [
          { name: 'Disk images', extensions: ['dd', 'img', 'raw', 'e01', 'E01', 'ex01', 'vhd', 'vhdx', 'vmdk', 'vdi', 'qcow2', 'qcow', 'dmg'] },
          { name: 'All files', extensions: ['*'] },
        ],
      }
      const result = focused
        ? await dialog.showOpenDialog(focused, opts)
        : await dialog.showOpenDialog(opts)
      if (result.canceled || result.filePaths.length === 0) return []
      return result.filePaths.filter((p) => isEvidenceImagePath(p)).slice(0, 16)
    } catch (err) {
      console.error('[IPC] pick-raid-member-images error:', err)
      return []
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

  // FAZ 1.3b: gallery thumbnail disk cache (L2) under userData/thumbs, served
  // to the renderer through the thumb:// protocol (see main.ts). The URL is
  // constructed here so the renderer never learns absolute cache paths.
  const thumbsDir = join(app.getPath('userData'), THUMB_DIR_NAME)

  ipcMain.handle('get-thumb-url', (_event, fileId: number, scanId: number) => {
    const fid = clampInt(fileId, 1, Number.MAX_SAFE_INTEGER, 0)
    const sid = clampInt(scanId, 1, Number.MAX_SAFE_INTEGER, 0)
    if (fid <= 0 || sid <= 0) return null
    const path = findThumbPath(thumbsDir, fid, sid)
    if (!path) return null
    return thumbUrlFor(fid, sid, mimeForExt(extname(path).slice(1).toLowerCase()) ?? '')
  })

  ipcMain.handle('put-thumb', (_event, fileId: number, scanId: number, mime: string, base64: string) => {
    try {
      const fid = clampInt(fileId, 1, Number.MAX_SAFE_INTEGER, 0)
      const sid = clampInt(scanId, 1, Number.MAX_SAFE_INTEGER, 0)
      if (fid <= 0 || sid <= 0 || typeof mime !== 'string' || typeof base64 !== 'string') return null
      // 512KB decoded cap: gallery previews are ≤64KB payloads; the cap only
      // guards a hostile/buggy renderer from ballooning the cache via IPC.
      if (base64.length > 700_000) return null
      const data = Buffer.from(base64, 'base64')
      if (data.length === 0 || data.length > 512 * 1024) return null
      storeThumb(thumbsDir, fid, sid, mime, data)
      return thumbUrlFor(fid, sid, mime)
    } catch (err) {
      console.error('[IPC] put-thumb error:', err)
      return null
    }
  })
}

