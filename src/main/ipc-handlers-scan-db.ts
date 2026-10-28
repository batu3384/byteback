import { ipcMain, IpcMainInvokeEvent } from 'electron'
import { getEngine } from './native-bridge'
import { callNative } from './ipc-native'
import { clampInt } from './ipc-utils'
import { assertDbReady, dbInitErrorMessage, isDbReady } from './ipc-db-state'
import { diskBusyMessage } from '../shared/scan-required'
import { CONTENT_SEARCH_MAX_QUERY_BYTES } from '../shared/content-search-status'
import { contentRegexQueryOk, CONTENT_REGEX_REJECTED_MESSAGE } from '../shared/content-regex-query'
import { appendSessionLog } from './session-log'

/** Scan DB pages, keyword/content search, and scan summary IPC. */
export function registerScanDbIpcHandlers(): void {
  ipcMain.handle('get-file-count', (_event, scanId: number, filter?: import('../shared/ipc-contract').FileListFilter) => {
    assertDbReady()
    return callNative('get-file-count', () =>
      getEngine().getFileCount(clampInt(scanId, 0, Number.MAX_SAFE_INTEGER, 0), filter)
    )
  })

  ipcMain.handle('get-files-page', (_event, scanId: number, offset: number, limit: number, filter?: import('../shared/ipc-contract').FileListFilter) => {
    assertDbReady()
    return callNative('get-files-page', () =>
      getEngine().getFilesPage(
        clampInt(scanId, 0, Number.MAX_SAFE_INTEGER, 0),
        clampInt(offset, 0, Number.MAX_SAFE_INTEGER, 0),
        clampInt(limit, 1, 1000, 100),
        filter,
      )
    )
  })

  ipcMain.handle('hash-empty-content', async (_event, scanId: number) => {
    try {
      assertDbReady()
      const id = clampInt(scanId, 0, Number.MAX_SAFE_INTEGER, 0)
      if (id <= 0) return { hashed: 0, error: 'Geçersiz tarama' }
      const res = await getEngine().hashEmptyContent(id)
      const hashed = typeof res?.hashed === 'number' && Number.isFinite(res.hashed) ? Math.max(0, res.hashed) : 0
      return { hashed, error: res?.error }
    } catch (err) {
      const raw = err instanceof Error ? err.message : String(err)
      const msg = diskBusyMessage(raw) ?? raw
      console.error('[IPC] hash-empty-content error:', err)
      return { hashed: 0, error: msg }
    }
  })

  ipcMain.handle('get-latest-scan-id', () =>
    callNative('get-latest-scan-id', () => getEngine().getLatestScanId())
  )

  ipcMain.handle('get-latest-usable-scan-id', () =>
    callNative('get-latest-usable-scan-id', () => getEngine().getLatestUsableScanId())
  )

  ipcMain.handle('reset-scan-database', () => {
    if (!isDbReady()) return false
    try {
      const ok = getEngine().resetScanDatabase()
      if (ok) appendSessionLog('DB_RESET', 'scan_data_cleared')
      return ok
    } catch (err) {
      console.error('[IPC] reset-scan-database error:', err)
      return false
    }
  })

  ipcMain.handle('get-scan-state', (_event, scanId: number) =>
    callNative('get-scan-state', () =>
      getEngine().getScanState(clampInt(scanId, 0, Number.MAX_SAFE_INTEGER, 0))
    )
  )

  ipcMain.handle('search-files', (_event, scanId: number, query: string, offset: number, limit: number, useRegex?: boolean, category?: string) => {
    try {
      if (useRegex && typeof query === 'string') {
        if (query.length > 128) {
          return { rows: [], error: 'Regex sorgusu en fazla 128 karakter olabilir.' }
        }
        if (!contentRegexQueryOk(query)) {
          return { rows: [], error: CONTENT_REGEX_REJECTED_MESSAGE }
        }
      }
      const engine = getEngine()
      const rows = engine.searchFiles(
        clampInt(scanId, 0, Number.MAX_SAFE_INTEGER, 0),
        query,
        clampInt(offset, 0, Number.MAX_SAFE_INTEGER, 0),
        clampInt(limit, 1, 1000, 100),
        !!useRegex,
        category ?? '',
      )
      return { rows }
    } catch (err) {
      console.error('[IPC] search-files error:', err)
      return { rows: [], error: err instanceof Error ? err.message : String(err) }
    }
  })

  ipcMain.handle('search-file-content', (_event, scanId: number, query: string, offset: number, limit: number, useRegex?: boolean) => {
    if (typeof query === 'string' && Buffer.byteLength(query, 'utf8') > CONTENT_SEARCH_MAX_QUERY_BYTES) {
      return { rows: [], error: 'content query too long' }
    }
    if (useRegex && typeof query === 'string' && !contentRegexQueryOk(query)) {
      return { rows: [], error: CONTENT_REGEX_REJECTED_MESSAGE }
    }
    try {
      const engine = getEngine()
      const rows = engine.searchFileContent(
        clampInt(scanId, 0, Number.MAX_SAFE_INTEGER, 0),
        query,
        clampInt(offset, 0, Number.MAX_SAFE_INTEGER, 0),
        clampInt(limit, 1, 1000, 100),
        !!useRegex,
      )
      return { rows }
    } catch (err) {
      console.error('[IPC] search-file-content error:', err)
      const raw = err instanceof Error ? err.message : String(err)
      return { rows: [], error: raw }
    }
  })

  ipcMain.handle('start-content-search', async (event: IpcMainInvokeEvent, scanId: number, query: string, useRegex?: boolean) => {
    if (!isDbReady()) {
      return { ok: false, error: dbInitErrorMessage() ?? 'Veritabanı kullanılamıyor' }
    }
    if (typeof query !== 'string' || Buffer.byteLength(query, 'utf8') > CONTENT_SEARCH_MAX_QUERY_BYTES) {
      return { ok: false, error: 'content query too long' }
    }
    if (useRegex && !contentRegexQueryOk(query)) {
      return { ok: false, error: CONTENT_REGEX_REJECTED_MESSAGE }
    }
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
      const ok = engine.startContentSearch(scanId, query, callback, !!useRegex)
      return { ok: !!ok }
    } catch (err) {
      const msg = err instanceof Error ? err.message : String(err)
      console.error('[IPC] start-content-search error:', err)
      return { ok: false, error: msg }
    }
  })

  ipcMain.on('stop-content-search', () => {
    try {
      getEngine().stopContentSearch()
    } catch (err) {
      console.error('[IPC] stop-content-search error:', err)
    }
  })

  ipcMain.handle('get-scan-summary', (_event, scanId: number) =>
    callNative('get-scan-summary', () => getEngine().getScanSummary(scanId))
  )
}
