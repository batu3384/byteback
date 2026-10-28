import { ipcMain } from 'electron'
import { getEngine } from './native-bridge'
import { assertDbReady } from './ipc-db-state'
import { assertBoundDriveIndex } from './ipc-guards'
import { destOnEvidenceError } from './ipc-evidence-context'
import { asciiForAudit, auditChainEvent } from './ipc-audit-bridge'
import { diskBusyMessage } from '../shared/scan-required'
import { parseRecoverIds, parseRecoverIdList } from '../shared/recover-ids'
import { validateRecoverDestDir } from './recover-dest-validator'
import { appendSessionLog } from './session-log'

export function registerRecoverIpcHandlers(): void {
  ipcMain.handle('recover-file', async (_event, driveIndex: number, fileId: number, destDir: string, scanId: number, preservePaths?: boolean) => {
    try {
      assertDbReady()
      if (driveIndex !== -1) assertBoundDriveIndex(driveIndex)
      if (!destDir || !destDir.trim()) {
        return { success: false, error: 'Hedef klasör seçilmedi' }
      }
      const parsed = parseRecoverIds(scanId, fileId)
      if (!parsed.ok) return { success: false, error: parsed.error }
      const dest = validateRecoverDestDir(destDir)
      if (!dest.ok) {
        appendSessionLog('RECOVER_FAIL', `scanId=${parsed.scanId} file=${parsed.fileId} dest_rejected: ${dest.error}`)
        return { success: false, error: dest.error }
      }
      const onSource = destOnEvidenceError(dest.destDir, driveIndex, parsed.scanId)
      if (onSource) {
        appendSessionLog('RECOVER_FAIL', `scanId=${parsed.scanId} file=${parsed.fileId} dest_rejected: ${onSource}`)
        return { success: false, error: onSource }
      }
      const engine = getEngine()
      const result = await engine.recoverFile(driveIndex, parsed.fileId, dest.destDir, parsed.scanId, preservePaths)
      if (!result.success) {
        appendSessionLog('RECOVER_FAIL', `scanId=${parsed.scanId} file=${parsed.fileId} error=${result.error ?? ''}`)
        auditChainEvent(`RECOVER_FAIL | scanId=${parsed.scanId} file=${parsed.fileId} reason=${asciiForAudit(result.error ?? 'unknown')}`)
      }
      return result
    } catch (err) {
      console.error('[IPC] recover-file error:', err)
      const raw = err instanceof Error ? err.message : String(err)
      return { success: false, error: diskBusyMessage(raw) ?? raw }
    }
  })

  ipcMain.handle('recover-files-batch', async (_event, driveIndex: number, fileIds: number[], destDir: string, scanId: number, preservePaths?: boolean) => {
    try {
      assertDbReady()
      if (driveIndex !== -1) assertBoundDriveIndex(driveIndex)
      if (!destDir || !destDir.trim()) {
        return { succeeded: 0, failed: fileIds?.length ?? 0, results: [], error: 'Hedef klasör seçilmedi' }
      }
      const parsed = parseRecoverIdList(scanId, fileIds)
      if (!parsed.ok) return { succeeded: 0, failed: fileIds?.length ?? 0, results: [], error: parsed.error }
      const dest = validateRecoverDestDir(destDir)
      if (!dest.ok) {
        appendSessionLog('RECOVER_FAIL', `scanId=${parsed.scanId} files=${parsed.fileIds.length} dest_rejected: ${dest.error}`)
        return { succeeded: 0, failed: parsed.fileIds.length, results: [], error: dest.error }
      }
      const onSource = destOnEvidenceError(dest.destDir, driveIndex, parsed.scanId)
      if (onSource) {
        appendSessionLog('RECOVER_FAIL', `scanId=${parsed.scanId} files=${parsed.fileIds.length} dest_rejected: ${onSource}`)
        return { succeeded: 0, failed: parsed.fileIds.length, results: [], error: onSource }
      }
      const engine = getEngine()
      const result = await engine.recoverFilesBatch(driveIndex, parsed.fileIds, dest.destDir, parsed.scanId, preservePaths)
      if (result.failed > 0) {
        const firstError = result.results.find((r) => !r.success)?.error ?? ''
        appendSessionLog('RECOVER_FAIL', `scanId=${parsed.scanId} failed=${result.failed}/${parsed.fileIds.length} error=${firstError}`)
        auditChainEvent(`RECOVER_FAIL | scanId=${parsed.scanId} failed=${result.failed}/${parsed.fileIds.length} reason=${asciiForAudit(firstError || 'unknown')}`)
      }
      return result
    } catch (err) {
      console.error('[IPC] recover-files-batch error:', err)
      const raw = err instanceof Error ? err.message : String(err)
      return { succeeded: 0, failed: fileIds?.length ?? 0, results: [], error: diskBusyMessage(raw) ?? raw }
    }
  })

  ipcMain.handle('read-file-preview', (_event, driveIndex: number, scanId: number, fileId: number) => {
    try {
      assertDbReady()
      if (driveIndex !== -1) assertBoundDriveIndex(driveIndex)
      const parsed = parseRecoverIds(scanId, fileId)
      if (!parsed.ok) return { success: false, error: parsed.error }
      const engine = getEngine()
      const res = engine.readFilePreview(driveIndex, parsed.scanId, parsed.fileId)
      if (res && res.error) return { ...res, error: diskBusyMessage(res.error) ?? res.error }
      if (res && res.success) {
        appendSessionLog('PREVIEW', `scanId=${parsed.scanId} file=${parsed.fileId} kind=${res.kind ?? ''} bytes=${res.data?.length ?? 0}`)
        auditChainEvent(`PREVIEW | scanId=${parsed.scanId} file=${parsed.fileId} kind=${asciiForAudit(res.kind ?? 'unknown')}`)
      }
      return res
    } catch (err) {
      console.error('[IPC] read-file-preview error:', err)
      const raw = err instanceof Error ? err.message : String(err)
      return { success: false, error: diskBusyMessage(raw) ?? raw }
    }
  })
}
