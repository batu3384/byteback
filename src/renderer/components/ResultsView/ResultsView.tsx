import React, { useState, useEffect, useCallback, useRef, useMemo } from 'react'
import './ResultsView.css'
import { File, FileImage, FileText, FileVideo, FileAudio, FileArchive, Download, ShieldCheck, Folder, FolderOpen, ListTree, List, Eye, LayoutGrid, Loader2, ChevronUp, ChevronDown, Binary } from 'lucide-react'
import type { FileRecord, FilePreviewResult, RaidState } from '../../../shared/ipc-contract'
import { localizeSourceLabel, isDiscoveryOnlySource, canRecoverSource, isRecoverableListSource, isDuplicateSource } from '../../../shared/source-label'
import { emptyScanHonestyFlags, loadScanHonestyFlags } from '../../../shared/scan-honesty'
import HonestyBanners from '../HonestyBanners'
import { diskBusyMessage } from '../../../shared/scan-required'
import { isDestOnScannedDrive, isDestOnRaidMemberDrive } from '../../../shared/recover-dest-guard'
import { previewDataUrl, resolvePreviewImageMime } from '../../../shared/preview-utils'
import { probeRaidState } from '../../../shared/hex-read'
import { useI18n, tFormat, formatInt } from '../../i18n'
import InlineAlert from '../InlineAlert'
import ResultsPreviewPanel from './ResultsPreviewPanel'
import {
  qualityHint,
  resolveFileTypeChip,
  formatSize,
  formatFsTimestamp,
  statusDisplayLabel,
  statusDisplayKey,
  buildTree,
  toSqlListFilter,
  confidenceTier,
  sortKey,
  cursorValueFor,
  type MappedFile,
  type TreeNode,
  type SortField,
  type SortDir,
  type PageCursor,
} from './results-view-utils'
import { computeVirtualWindow, estimateRowHeight, DEFAULT_ROW_HEIGHT, ROW_OVERSCAN, type VirtualWindow } from './virtual-rows'

interface ResultsViewProps {
  filesFound: any[]
  driveIndex: number | null
  scanId?: number
  scanBusy?: boolean
  onShowMft?: (mftRef: number) => void
}

const PAGE_SIZE = 500

// W3: module-level so React keeps card state across parent re-renders.
function ThumbCard({ f, thumb, thumbUrl, onVisible, onOpen, noPreviewLabel, ariaLabel }: {
  f: MappedFile
  thumb?: FilePreviewResult
  /** FAZ 1.3b L2: thumb:// URL served from the main-process disk cache. */
  thumbUrl?: string | null
  onVisible: (id: number) => void
  onOpen: (id: number) => void
  noPreviewLabel: string
  ariaLabel: string
}): React.ReactElement {
  const [visible, setVisible] = useState(false)
  const imgRef = useRef<HTMLDivElement | null>(null)
  // W4 cap eviction can drop an already-loaded thumb (clear-all at 1000);
  // re-arm the lazy observer, or visible stays true and the card spins forever.
  const prevThumbRef = useRef<FilePreviewResult | undefined>(thumb)
  if (prevThumbRef.current !== thumb) {
    prevThumbRef.current = thumb
    if (!thumb) setVisible(false)
  }
  useEffect(() => {
    const el = imgRef.current
    if (!el || visible) return
    const io = new IntersectionObserver(
      (entries) => {
        if (entries.some((en) => en.isIntersecting)) {
          setVisible(true)
          onVisible(f.id)
        }
      },
      { rootMargin: '200px' },
    )
    io.observe(el)
    return () => io.disconnect()
  }, [visible, f.id, onVisible])
  const dataUrl = useMemo(
    () => (thumb && thumb.kind === 'image' ? previewDataUrl(thumb) : null),
    [thumb],
  )
  // L1 (in-memory data URL) wins when both exist; thumb:// covers the L2 hit.
  const imgSrc = dataUrl ?? (thumbUrl || null)
  return (
    <div
      ref={imgRef}
      className="results-thumb"
      onClick={() => onOpen(f.id)}
      role="button"
      tabIndex={0}
      onKeyDown={(e) => {
        if (e.key === 'Enter' || e.key === ' ') onOpen(f.id)
      }}
      aria-label={ariaLabel}
    >
      <div className="results-thumb-frame">
        {imgSrc ? (
          <img src={imgSrc} alt={f.name} loading="lazy" />
        ) : thumb ? (
          <span className="results-thumb-empty">{noPreviewLabel}</span>
        ) : (
          <Loader2 size={20} className="spinner" color="var(--text-muted)" />
        )}
      </div>
      <div className="results-thumb-name" title={f.name}>
        {f.name}
      </div>
    </div>
  )
}

function ResultsView({ filesFound, driveIndex, scanId, scanBusy, onShowMft }: ResultsViewProps): React.ReactElement {
  const { t } = useI18n()
  const [statusFilter, setStatusFilter] = useState<'deleted' | 'all' | 'allocated' | 'carved'>('deleted')
  const [typeFilter, setTypeFilter] = useState('all')
  const [nameInput, setNameInput] = useState('')
  const [nameQuery, setNameQuery] = useState('')
  const [summary, setSummary] = useState({ totalFiles: 0, deletedFiles: 0, carvedFiles: 0 })
  const [showDuplicates, setShowDuplicates] = useState(false)
  const [selectedFiles, setSelectedFiles] = useState<Set<number>>(new Set())
  const [isRecovering, setIsRecovering] = useState(false)
  const [viewMode, setViewMode] = useState<'tree' | 'flat' | 'gallery'>('flat')
  const [expandedDirs, setExpandedDirs] = useState<Set<string>>(new Set())
  const [dbFiles, setDbFiles] = useState<FileRecord[]>([])
  const [totalCount, setTotalCount] = useState(0)
  const [page, setPage] = useState(0)
  const [loading, setLoading] = useState(false)
  const [listError, setListError] = useState(false)
  const [recordById, setRecordById] = useState<Map<number, FileRecord>>(new Map())
  const [honesty, setHonesty] = useState(emptyScanHonestyFlags)
  const [honestyLoadFailed, setHonestyLoadFailed] = useState(false)
  const [recoverReport, setRecoverReport] = useState<string | null>(null)
  const [recoverStats, setRecoverStats] = useState<{ failed: number; zero: number; bad: number } | null>(null)
  const [preview, setPreview] = useState<FilePreviewResult | null>(null)
  const [previewLoading, setPreviewLoading] = useState(false)
  const [previewTargetId, setPreviewTargetId] = useState<number | null>(null)
  const [sortField, setSortField] = useState<SortField>('confidence')
  const [sortDir, setSortDir] = useState<SortDir>('desc')
  // P0-1: restore the original folder tree under destDir.
  const [preservePaths, setPreservePaths] = useState(true)
  // P0-4: size (bytes) and date (unix seconds) filter state.
  const [sizeMinInput, setSizeMinInput] = useState('')
  const [sizeMaxInput, setSizeMaxInput] = useState('')
  const [dateFromInput, setDateFromInput] = useState('')
  const [dateToInput, setDateToInput] = useState('')
  const [thumbs, setThumbs] = useState<Map<number, FilePreviewResult>>(new Map())
  const thumbsRef = useRef(thumbs)
  thumbsRef.current = thumbs
  // FAZ 1.3b L2: thumb:// URLs answered from the main-process disk cache
  // (userData/thumbs); fileIds here need no drive read to render.
  const [thumbUrls, setThumbUrls] = useState<Map<number, string>>(new Map())
  const thumbUrlsRef = useRef(thumbUrls)
  thumbUrlsRef.current = thumbUrls
  const thumbLoadingRef = useRef<Set<number>>(new Set())
  const previewReqRef = useRef(0)
  const [csvExporting, setCsvExporting] = useState(false)
  const [exportError, setExportError] = useState<string | null>(null)
  // FAZ 1.3c: success surface with the natively written row count + path.
  const [csvReport, setCsvReport] = useState<string | null>(null)
  const [hashingContent, setHashingContent] = useState(false)
  const [hashReport, setHashReport] = useState<string | null>(null)
  const loadGenRef = useRef(0)
  // FAZ 1.2 keyset pagination: last row (native sort-key value + id) of each
  // loaded page; fetching page N attaches page N-1's cursor to the filter.
  // Page jumps without the predecessor entry fall back to OFFSET (null).
  const pageCursorRef = useRef<Map<number, PageCursor>>(new Map())
  // FAZ 1.3a table virtualization: only the scroll window (+overscan) renders;
  // spacer rows keep the scrollbar geometry honest. Flat/table mode only.
  const scrollRef = useRef<HTMLDivElement | null>(null)
  const rowProbeRef = useRef<HTMLTableRowElement | null>(null)
  const [rowHeight, setRowHeight] = useState(DEFAULT_ROW_HEIGHT)
  const [vWindow, setVWindow] = useState<VirtualWindow>({ start: 0, end: 0 })

  const effectiveScanId = scanId && scanId > 0 ? scanId : -1

  const loadPreview = async (fileId: number) => {
    if (scanBusy) {
      setPreview({ success: false, error: t('results.previewWhileBusy') })
      return
    }
    if (effectiveScanId <= 0 || !window.api?.readFilePreview) {
      setPreview({ success: false, error: t('results.previewNeedsScan') })
      return
    }
    const raidProbe = await probeRaidState(window.api?.getRaidState)
    const effectiveDrive = driveIndex !== null ? driveIndex : -1
    if (effectiveDrive < 0) {
      if (raidProbe.status === 'unread') {
        setPreview({ success: false, error: t('results.raidStateFailed') })
        return
      }
      if (!raidProbe.state.active) {
        setPreview({ success: false, error: t('results.previewNeedsDrive') })
        return
      }
    }
    // CA-028: generation guard — rapid A/B clicks can resolve out of order;
    // only the latest request may touch the panel.
    const gen = ++previewReqRef.current
    setPreviewLoading(true)
    setPreviewTargetId(fileId)
    try {
      const res = await window.api.readFilePreview(effectiveDrive, effectiveScanId, fileId)
      if (gen !== previewReqRef.current) return
      const err = diskBusyMessage(res.error) ?? res.error
      setPreview(err && err !== res.error ? { ...res, error: err } : res)
    } catch (e) {
      if (gen !== previewReqRef.current) return
      const raw = e instanceof Error ? e.message : String(e)
      setPreview({ success: false, error: diskBusyMessage(raw) ?? t('results.previewReadFailed') })
    } finally {
      if (gen === previewReqRef.current) setPreviewLoading(false)
    }
  }

  // P0-4: numeric filter values (MB inputs -> bytes; date inputs -> unix),
  // debounced like nameQuery — deriving them directly re-fetched the page on
  // every keystroke in the size inputs.
  const [sizeMin, setSizeMin] = useState(0)
  const [sizeMax, setSizeMax] = useState(0)
  const [dateFrom, setDateFrom] = useState(0)
  const [dateTo, setDateTo] = useState(0)

  useEffect(() => {
    const t = setTimeout(() => {
      const size = (input: string) => {
        if (!input.trim()) return 0
        const bytes = Math.round(parseFloat(input) * 1024 * 1024)
        return Number.isFinite(bytes) ? bytes : 0
      }
      const date = (input: string, endOfDay: boolean) => {
        if (!input.trim()) return 0
        const sec = Math.floor(new Date(input).getTime() / 1000)
        return Number.isFinite(sec) && sec > 0 ? sec + (endOfDay ? 86399 : 0) : 0
      }
      setSizeMin(size(sizeMinInput))
      setSizeMax(size(sizeMaxInput))
      setDateFrom(date(dateFromInput, false))
      setDateTo(date(dateToInput, true))
    }, 300)
    return () => clearTimeout(t)
  }, [sizeMinInput, sizeMaxInput, dateFromInput, dateToInput])

  const loadPage = useCallback(async (scan: number, pageIndex: number) => {
    if (!window.api?.getFilesPage || !window.api?.getFileCount || scan <= 0) return
    const gen = ++loadGenRef.current
    setLoading(true)
    const listFilter = toSqlListFilter(statusFilter, typeFilter, nameQuery, showDuplicates, sortKey(sortField, sortDir),
      { sizeMin: sizeMin > 0 ? sizeMin : undefined, sizeMax: sizeMax > 0 ? sizeMax : undefined,
        dateFrom: dateFrom > 0 ? dateFrom : undefined, dateTo: dateTo > 0 ? dateTo : undefined,
        // Keyset cursor from the previous sequential page; first page and
        // page jumps send null — native then keeps the OFFSET path. Offset
        // itself still travels for old-native compatibility (FAZ 1.2).
        cursor: pageIndex > 0 ? pageCursorRef.current.get(pageIndex - 1) ?? null : null })
    try {
      const [count, pageData, sum] = await Promise.all([
        window.api.getFileCount(scan, listFilter),
        window.api.getFilesPage(scan, pageIndex * PAGE_SIZE, PAGE_SIZE, listFilter),
        window.api.getScanSummary ? window.api.getScanSummary(scan) : Promise.resolve(null),
      ])
      if (gen !== loadGenRef.current) return
      setTotalCount(typeof count === 'number' && count >= 0 ? count : 0)
      setDbFiles(pageData ?? [])
      setListError(false)
      // Remember this page's last row so the next sequential fetch rides the
      // keyset cursor (gen guard above keeps stale fetches from writing it).
      if (pageData && pageData.length > 0) {
        const last = pageData[pageData.length - 1]!
        pageCursorRef.current.set(pageIndex, { v: cursorValueFor(sortField, last), id: last.id })
      }
      if (sum) {
        setSummary({
          totalFiles: sum.totalFiles ?? 0,
          deletedFiles: sum.deletedFiles ?? 0,
          carvedFiles: sum.carvedFiles ?? 0,
        })
      }
      setRecordById(prev => {
        const next = new Map(prev)
        for (const f of pageData ?? []) next.set(f.id, f)
        return next
      })
    } catch {
      if (gen !== loadGenRef.current) return
      // A DB failure must surface as an error, not as an empty-but-healthy
      // list that reads as "the scan found nothing".
      setDbFiles([])
      setTotalCount(0)
      setListError(true)
    } finally {
      if (gen === loadGenRef.current) setLoading(false)
    }
  }, [statusFilter, typeFilter, nameQuery, showDuplicates, sortField, sortDir, sizeMin, sizeMax, dateFrom, dateTo])

  useEffect(() => {
    const t = setTimeout(() => setNameQuery(nameInput.trim()), 300)
    return () => clearTimeout(t)
  }, [nameInput])

  useEffect(() => {
    setPage(0)
    setSelectedFiles(new Set())
    // Cursors belong to a (sort, filter) combination — a change invalidates all.
    pageCursorRef.current.clear()
  }, [statusFilter, typeFilter, nameQuery, showDuplicates, sizeMin, sizeMax, dateFrom, dateTo])

  useEffect(() => {
    const maxPage = totalCount <= 0 ? 0 : Math.max(0, Math.ceil(totalCount / PAGE_SIZE) - 1)
    if (page > maxPage) setPage(maxPage)
  }, [totalCount, page])

  useEffect(() => {
    if (effectiveScanId > 0) {
      loadPage(effectiveScanId, page)
    } else {
      setDbFiles([])
      setTotalCount(filesFound.length)
    }
  }, [effectiveScanId, page, loadPage, filesFound.length])

  useEffect(() => {
    const localRows = effectiveScanId > 0 ? dbFiles : filesFound
    let cancelled = false
    void loadScanHonestyFlags(effectiveScanId, window.api?.getFilesPage, localRows)
      .then((flags) => {
        if (cancelled) return
        setHonestyLoadFailed(false)
        setHonesty(flags)
      })
      .catch(() => {
        if (cancelled) return
        setHonestyLoadFailed(true)
      })
    return () => { cancelled = true }
  }, [effectiveScanId, dbFiles, filesFound])

  const sourceFiles: FileRecord[] = effectiveScanId > 0
    ? dbFiles
    : filesFound.map((f, i) => ({
        id: typeof f.id === 'number' ? f.id : i,
        name: f.name ?? '',
        sizeBytes: f.sizeBytes ?? f.size ?? 0,
        status: f.status ?? 0,
        path: f.path,
        category: f.category,
        confidence: f.confidence,
        startSector: f.startSector,
        endSector: f.endSector,
        createdAt: f.createdAt,
        modifiedAt: f.modifiedAt,
        runs: f.runs,
        source: f.source,
        mftRef: f.mftRef,
      })).filter((f) => isRecoverableListSource(f.source) || (showDuplicates && isDuplicateSource(f.source)))

  const runRecover = async (destDir: string, effectiveDrive: number, raidState: RaidState): Promise<void> => {
    try {
    const filesToRecover: FileRecord[] = []
    const skipped: string[] = []
    for (const id of selectedFiles) {
      const fileToRecover = recordById.get(id) ?? sourceFiles.find(f => f.id === id)
      if (!fileToRecover) continue
      const hasRuns = (fileToRecover.runs?.length ?? 0) > 0
      if (!canRecoverSource(fileToRecover.source, hasRuns) || isDiscoveryOnlySource(fileToRecover.source)) {
        skipped.push(`${fileToRecover.name} (${localizeSourceLabel(fileToRecover.source, t)})`)
        continue
      }
      filesToRecover.push(fileToRecover)
    }
    const fileIds = filesToRecover.map((f) => f.id).filter((id) => id > 0)
    if (fileIds.length === 0) {
      setRecoverReport(
        skipped.length
          ? tFormat('results.recoverSkippedList', { list: skipped.join('\n') })
          : t('results.recoverNoIds'),
      )
      return
    }

    let successCount = 0
    let failedCount = 0
    let zeroFilledCount = 0
    let validatedOk = 0
    let validatedBad = 0
    const errors: string[] = []
    // CA-042: MD5 hashes are success info, not errors — reported separately.
    const verified: string[] = []

    const noteValidation = (res: { validationScore?: number }) => {
      if (typeof res.validationScore !== 'number' || res.validationScore < 0) return
      if (res.validationScore >= 60) validatedOk++
      else validatedBad++
    }

    const noteResult = async (res: { success?: boolean; zeroFilled?: boolean; error?: string; validationScore?: number; validationError?: string; md5Hash?: string; destPath?: string; repairedPath?: string }, id: number) => {
      if (res.success) successCount++
      else {
        failedCount++
        if (res.error) errors.push(`#${id}: ${res.error}`)
      }
      if (res.zeroFilled) zeroFilledCount++
      if (res.validationError) errors.push(tFormat('results.validationError', { id: String(id), err: res.validationError }))
      if (res.repairedPath) verified.push(tFormat('results.repairedLine', { id: String(id), path: res.repairedPath }))
      if (res.md5Hash) {
        let nsrlLine = `#${id} MD5: ${res.md5Hash}`
        if (window.api.lookupNsrl) {
          try {
            const known = await window.api.lookupNsrl(res.md5Hash)
            if (known) nsrlLine += t('results.nsrlKnown')
          } catch {
            nsrlLine += t('results.nsrlUnread')
          }
        }
        verified.push(nsrlLine)
      }
      noteValidation(res)
    }

    if (fileIds.length > 1 && window.api.recoverFilesBatch) {
      try {
        const res = await window.api.recoverFilesBatch(
          effectiveDrive,
          fileIds,
          destDir,
          effectiveScanId,
          preservePaths,
        )
        if (res.error) errors.push(diskBusyMessage(res.error) ?? res.error)
        for (let i = 0; i < (res.results ?? []).length; ++i) {
          const r = res.results![i]!
          const fid = fileIds[i] ?? 0
          await noteResult(r, fid)
        }
      } catch (e) {
        failedCount = fileIds.length
        const raw = e instanceof Error ? e.message : String(e)
        errors.push(diskBusyMessage(raw) ?? t('results.batchException'))
      }
    } else {
      for (const fileId of fileIds) {
        try {
          const res = await window.api.recoverFile(
            effectiveDrive,
            fileId,
            destDir,
            effectiveScanId,
            preservePaths,
          )
          await noteResult(res, fileId)
        } catch {
          failedCount++
          errors.push(tFormat('results.idException', { id: String(fileId) }))
        }
      }
    }

    setIsRecovering(false)
    const skipLine = skipped.length ? tFormat('results.skipLine', { n: String(skipped.length) }) : ''
    const padWarn =
      zeroFilledCount > 0
        ? tFormat('results.padWarn', { n: String(zeroFilledCount) })
        : ''
    const validationLine =
      validatedOk + validatedBad > 0
        ? tFormat('results.validationLine', { ok: String(validatedOk), bad: String(validatedBad) }) +
            (validatedBad > 0 ? t('results.validationBadNote') : '')
        : ''
    const verifiedLine = verified.length ? tFormat('results.verifiedLine', { list: verified.slice(0, 8).join('\n') }) : ''
    const errLine = errors.length ? tFormat('results.errLine', { list: errors.slice(0, 8).join('\n') }) : ''
    setRecoverReport(
      tFormat('results.recoverDone', { ok: String(successCount), bad: String(failedCount), zero: String(zeroFilledCount), dest: destDir }) + `${skipLine}${padWarn}${validationLine}${verifiedLine}${errLine}`,
    )
    setRecoverStats({ failed: failedCount, zero: zeroFilledCount, bad: validatedBad })
    } finally {
      setIsRecovering(false)
    }
  }

  const handleRecover = async () => {
    if (scanBusy) {
      setRecoverReport(t('results.recoverWhileBusy'))
      return
    }
    if (effectiveScanId <= 0) {
      setRecoverReport(t('results.recoverNeedsScan'))
      return
    }
    if (selectedFiles.size === 0) return
    const raidProbe = await probeRaidState(window.api?.getRaidState)
    if (raidProbe.status === 'unread') {
      setRecoverReport(t('results.raidStateFailed'))
      return
    }
    const raidState = raidProbe.state
    const effectiveDrive = driveIndex !== null ? driveIndex : -1
    if (effectiveDrive < 0 && !raidState.active) {
      setRecoverReport(t('results.recoverNeedsDrive'))
      return
    }
    if (!window.api?.recoverFile) {
      setRecoverReport(t('results.recoverNoApi'))
      return
    }

    setIsRecovering(true)
    setRecoverReport(null)
    setRecoverStats(null)
    try {
      const destDir = await window.api.pickDirectory()
      if (!destDir) return
      if (
        effectiveDrive >= 0 &&
        window.api.resolveVolume &&
        (await isDestOnScannedDrive(destDir, effectiveDrive, (letter) => window.api.resolveVolume(letter)))
      ) {
        setRecoverReport(t('results.destOnSourceBlocked'))
        return
      }
      if (
        raidState.active &&
        window.api.resolveVolume &&
        (await isDestOnRaidMemberDrive(
          destDir,
          raidState.memberDriveIndices ?? [],
          (letter) => window.api.resolveVolume(letter),
        ))
      ) {
        setRecoverReport(t('results.destOnRaidBlocked'))
        return
      }
      await runRecover(destDir, effectiveDrive, raidState)
    } finally {
      setIsRecovering(false)
    }
  }

  const handlePreviewSelected = () => {
    if (selectedFiles.size !== 1) return
    const id = Array.from(selectedFiles)[0]!
    void loadPreview(id)
  }

  const previewRecord =
    previewTargetId != null
      ? recordById.get(previewTargetId) ?? sourceFiles.find((f) => f.id === previewTargetId)
      : undefined

  const toggleSelection = (id: number) => {
    const newSel = new Set(selectedFiles)
    if (newSel.has(id)) newSel.delete(id)
    else newSel.add(id)
    setSelectedFiles(newSel)
  }

  const exportCsv = async () => {
    if (effectiveScanId <= 0 || !window.api?.exportCsv || csvExporting) return
    // Same filter shape as loadPage (minus the paging cursor — native streams
    // the whole matching set in one pass, no keyset needed).
    const listFilter = toSqlListFilter(statusFilter, typeFilter, nameQuery, showDuplicates, sortKey(sortField, sortDir),
      { sizeMin: sizeMin > 0 ? sizeMin : undefined, sizeMax: sizeMax > 0 ? sizeMax : undefined,
        dateFrom: dateFrom > 0 ? dateFrom : undefined, dateTo: dateTo > 0 ? dateTo : undefined })
    setCsvExporting(true)
    setExportError(null)
    setCsvReport(null)
    try {
      // FAZ 1.3c: the native engine streams the CSV in one prepared SELECT
      // (replacing the old 100-round offset walk) and the save dialog lives in
      // the main process — the renderer never sends a file path.
      const header = ['name', 'sizeBytes', 'category', 'confidence', 'status', 'path', 'source', 'startSector', 'createdAt', 'modifiedAt'].map((k) => t(`csv.${k}`))
      const suggestedName = tFormat('results.csvFileName', { n: String(filteredFiles.length), date: new Date().toISOString().slice(0, 10) })
      const res = await window.api.exportCsv(
        effectiveScanId,
        listFilter,
        header,
        { noFsDate: t('ts.noFsDate'), noDate: '—' },
        suggestedName,
      )
      if (res.canceled) return
      if (!res.success || typeof res.rows !== 'number') throw new Error(res.error ?? 'csv export failed')
      // Row count when done: the export ran natively, the user only sees the dialog.
      setCsvReport(tFormat('results.csvDone', { n: formatInt(res.rows), path: res.path ?? '' }))
    } catch {
      // In-app error surface — window.alert would break the modal pattern.
      setExportError(t('results.csvFailed'))
    } finally {
      setCsvExporting(false)
    }
  }

  const analyzeContentHash = async () => {
    if (effectiveScanId <= 0 || !window.api?.hashEmptyContent || hashingContent || scanBusy) return
    setHashingContent(true)
    setHashReport(null)
    setExportError(null)
    try {
      const res = await window.api.hashEmptyContent(effectiveScanId)
      if (res?.error) throw new Error(res.error)
      const n = typeof res?.hashed === 'number' ? res.hashed : 0
      setHashReport(tFormat('results.analyzeDedupDone', { n: formatInt(n) }))
      await loadPage(effectiveScanId, page)
    } catch (err) {
      const msg = err instanceof Error ? err.message : String(err)
      setExportError(msg || t('results.analyzeDedupFailed'))
    } finally {
      setHashingContent(false)
    }
  }

  const mappedFiles: MappedFile[] = sourceFiles.map((f) => ({
    id: f.id,
    name: f.name,
    rawPath: typeof f.path === 'string' ? f.path : '',
    rawStatus: f.status ?? 0,
    size: formatSize(f.sizeBytes || 0),
    path: typeof f.path === 'string' && f.path ? f.path : '—',
    status: statusDisplayLabel(f.status, f.source),
    statusKey: statusDisplayKey(f.status, f.source),
    type: resolveFileTypeChip(f),
    sourceLabel: localizeSourceLabel(f.source, t),
    dateLabel: formatFsTimestamp(f.modifiedAt || f.createdAt, f.source),
    qualityLabel: qualityHint(f),
    sameContentGroup: (f.contentGroupSize ?? 0) >= 2 ? (f.contentGroupSize ?? 0) : 0,
    confidence: f.confidence,
    confidenceTier: confidenceTier(f.confidence),
    mftRef: typeof f.mftRef === 'number' && Number.isInteger(f.mftRef) && f.mftRef >= 0 ? f.mftRef : undefined,
  }))

  const filteredFiles = mappedFiles
  const treeRoot = buildTree(filteredFiles)
  const galleryFiles = filteredFiles.filter((f) => f.type === 'img')
  // Unread list ≠ empty filter: the banner already carries the error; the
  // table/gallery/tree must not also claim "no files in this filter".
  const listEmptyMessage = listError
    ? t('results.loadErrorTitle')
    : loading
      ? t('results.loading')
      : t('results.empty')
  const galleryEmptyMessage = listError
    ? t('results.loadErrorTitle')
    : loading
      ? t('results.loading')
      : t('results.noImages')

  // FAZ 1.3a — virtual window for the flat table (gallery/tree keep their own
  // dynamics and render everything, as before).
  const recomputeWindow = useCallback(() => {
    const el = scrollRef.current
    if (!el || viewMode !== 'flat') return
    setVWindow(computeVirtualWindow({
      scrollTop: el.scrollTop,
      viewportHeight: el.clientHeight,
      rowHeight,
      totalRows: filteredFiles.length,
      overscan: ROW_OVERSCAN,
    }))
  }, [viewMode, rowHeight, filteredFiles.length])

  // Page / sort / filter changes restart the list at the top: reset scrollTop
  // (its scroll event recomputes the window) and recompute for the fresh rows.
  useEffect(() => {
    const el = scrollRef.current
    if (el && el.scrollTop !== 0) el.scrollTop = 0
    recomputeWindow()
  }, [page, effectiveScanId, viewMode, recomputeWindow])

  // Measure the real row height from the first rendered row (probe ref);
  // jsdom/collapsed tables report 0 and keep the 40px fallback via estimateRowHeight.
  useEffect(() => {
    if (viewMode !== 'flat') return
    const h = estimateRowHeight(rowProbeRef.current?.getBoundingClientRect().height)
    setRowHeight((prev) => (Math.abs(prev - h) > 0.5 ? h : prev))
  })

  const visibleFiles = viewMode === 'flat' ? filteredFiles.slice(vWindow.start, vWindow.end) : filteredFiles

  // CA-037: select-all means "all rows visible on this page". Selection
  // itself persists across pages; the checkbox only reflects this page.
  const allPageSelected = filteredFiles.length > 0 && filteredFiles.every((f) => selectedFiles.has(f.id))
  const toggleAll = (e: React.ChangeEvent<HTMLInputElement>) => {
    if (e.target.checked) {
      setSelectedFiles(new Set([...selectedFiles, ...filteredFiles.map((f) => f.id)]))
    } else {
      const next = new Set(selectedFiles)
      for (const f of filteredFiles) next.delete(f.id)
      setSelectedFiles(next)
    }
  }

  const toggleSort = (field: SortField) => {
    if (sortField === field) {
      setSortDir((d) => (d === 'asc' ? 'desc' : 'asc'))
    } else {
      setSortField(field)
      setSortDir(field === 'name' || field === 'path' ? 'asc' : 'desc')
    }
    setPage(0)
    pageCursorRef.current.clear()
  }

  const sortIndicator = (field: SortField): React.ReactNode =>
    sortField === field ? (
      sortDir === 'asc'
        ? <ChevronUp size={14} className="sort-ico" />
        : <ChevronDown size={14} className="sort-ico" />
    ) : null

  // Sortable column headers are keyboard-operable and expose aria-sort.
  const sortableTh = (field: SortField) => ({
    tabIndex: 0 as const,
    'aria-sort': (sortField === field ? (sortDir === 'asc' ? 'ascending' : 'descending') : 'none') as 'ascending' | 'descending' | 'none',
    onClick: () => toggleSort(field),
    onKeyDown: (e: React.KeyboardEvent) => {
      if (e.key === 'Enter' || e.key === ' ') {
        e.preventDefault()
        toggleSort(field)
      }
    },
  })

  const loadThumb = useCallback(async (id: number) => {
    if (thumbsRef.current.has(id) || thumbUrlsRef.current.has(id) || thumbLoadingRef.current.has(id)) return
    if (effectiveScanId <= 0) return
    thumbLoadingRef.current.add(id)
    try {
      // FAZ 1.3b L2: a disk cache hit renders through thumb:// with no drive
      // read, so a gallery survives an app restart without the imaged disk.
      if (window.api?.getThumbUrl) {
        const cached = await window.api.getThumbUrl(id, effectiveScanId)
        if (cached) {
          setThumbUrls((prev) => new Map(prev).set(id, cached))
          return
        }
      }
      const raidProbe = await probeRaidState(window.api?.getRaidState)
      const effectiveDrive = driveIndex !== null ? driveIndex : -1
      if (effectiveDrive < 0) {
        if (raidProbe.status === 'unread' || !raidProbe.state.active) return
      }
      if (!window.api?.readFilePreview) return
      const res = await window.api.readFilePreview(effectiveDrive, effectiveScanId, id)
      setThumbs((prev) => {
        const next = new Map(prev)
        // W4: 64KB payloads must not accumulate unbounded across pages.
        // ponytail: clear-all eviction at 1000 — LRU needs age tracking we
        // don't have; a hard cap keeps the renderer bounded.
        if (next.size >= 1000) next.clear()
        next.set(id, res)
        return next
      })
      // FAZ 1.3b: populate the disk cache (L2) from the fetched preview so the
      // next session rides thumb:// instead of re-reading the drive.
      if (window.api?.putThumb && res.success && res.data?.length) {
        const mime = resolvePreviewImageMime(res)
        const dataUrl = mime ? previewDataUrl(res) : null
        const prefix = mime ? `data:${mime};base64,` : ''
        if (mime && dataUrl?.startsWith(prefix)) {
          const url = await window.api.putThumb(id, effectiveScanId, mime, dataUrl.slice(prefix.length))
          if (url) setThumbUrls((prev) => new Map(prev).set(id, url))
        }
      }
    } catch {
      /* thumbnail is best-effort */
    } finally {
      thumbLoadingRef.current.delete(id)
    }
  }, [driveIndex, effectiveScanId])

  // W4: a different scan session invalidates every cached preview/record.
  useEffect(() => {
    setThumbs(new Map())
    setThumbUrls(new Map())
    setRecordById(new Map())
    thumbLoadingRef.current.clear()
    pageCursorRef.current.clear()
  }, [effectiveScanId])

  // Gallery thumbnail card: lazy-loads its 64KB preview when scrolled into view.
  // (W3: the card component lives at module level — an inline definition gave
  // it a new identity every render and remounted the whole gallery per state
  // change, O(N²) while thumbnails arrived.)

  const renderTreeNode = (node: TreeNode, depth: number): React.ReactNode[] => {
    const out: React.ReactNode[] = []
    const sortedDirs = Array.from(node.dirs.values()).sort((a, b) => a.name.localeCompare(b.name, 'tr'))
    for (const dir of sortedDirs) {
      const isOpen = expandedDirs.has(dir.path)
      const childCount = dir.files.length + dir.dirs.size
      out.push(
        <button
          type="button"
          key={'d:' + dir.path}
          aria-expanded={isOpen}
          onClick={() => {
            const next = new Set(expandedDirs)
            if (next.has(dir.path)) next.delete(dir.path)
            else next.add(dir.path)
            setExpandedDirs(next)
          }}
          className="tree-dir"
          style={{ '--indent': `${depth * 16}px` } as React.CSSProperties}
        >
          {isOpen ? <FolderOpen size={16} color="var(--accent-blue)" /> : <Folder size={16} color="var(--accent-blue)" />}
          <span className="tree-dir-name">{dir.name}</span>
          <span className="tree-dir-count">{tFormat('results.items', { n: String(childCount) })}</span>
        </button>
      )
      if (isOpen) out.push(...renderTreeNode(dir, depth + 1))
    }
    for (const f of node.files) {
      out.push(
        <button
          type="button"
          key={'f:' + f.id}
          aria-pressed={selectedFiles.has(f.id)}
          onClick={() => toggleSelection(f.id)}
          className={`tree-file${selectedFiles.has(f.id) ? ' is-selected' : ''}`}
          style={{ '--indent': `${(depth + 1) * 16}px` } as React.CSSProperties}
        >
          <input type="checkbox" className="tree-check" checked={selectedFiles.has(f.id)} onChange={() => toggleSelection(f.id)} onClick={(e) => e.stopPropagation()} aria-hidden="true" tabIndex={-1} />
          {getIconForType(f.type)}
          <span className="tree-file-name">{f.name}</span>
          {f.sameContentGroup >= 2 ? (
            <span className="same-content-badge" title={tFormat('results.sameContentTitle', { n: String(f.sameContentGroup) })}>
              {t('results.sameContent')}
            </span>
          ) : null}
          {f.sourceLabel ? <span className="tree-file-src">{f.sourceLabel}</span> : null}
          <span className="tree-file-size">{f.size}</span>
        </button>
      )
    }
    return out
  }

  const getIconForType = (type: string) => {
    switch (type) {
      case 'img': return <FileImage size={18} color="var(--accent-blue)" />
      case 'doc': return <FileText size={18} color="var(--success-green)" />
      case 'video': return <FileVideo size={18} color="var(--alert-red)" />
      case 'audio': return <FileAudio size={18} color="var(--warning-yellow)" />
      case 'archive': return <FileArchive size={18} color="var(--accent-blue)" />
      default: return <File size={18} color="var(--text-muted)" />
    }
  }

  const displayTotal = effectiveScanId > 0 ? totalCount : filesFound.length
  const totalPages = Math.max(1, Math.ceil(displayTotal / PAGE_SIZE))

  return (
    <div className="results-view">
      <div className="results-header glass-panel">
        <div className="results-info">
          <h2>{t('results.title')}</h2>
          <p>
            {tFormat('results.inFilterCount', { n: formatInt(displayTotal) })}
            {effectiveScanId > 0 && totalPages > 1 ? tFormat('results.pageOf', { cur: String(page + 1), total: String(totalPages) }) : ''}
            {loading ? t('results.loadingShort') : ''}
          </p>
          {effectiveScanId > 0 && (
            <p className="results-summary">
              {tFormat('results.deletedCount', { n: formatInt(summary.deletedFiles) })}
              {' · '}{tFormat('results.allocatedCount', { n: formatInt(Math.max(0, summary.totalFiles - summary.deletedFiles - (summary.carvedFiles ?? 0))) })}
              {' · '}{tFormat('results.carvedCount', { n: formatInt(summary.carvedFiles ?? 0) })}
              {' · '}{tFormat('results.totalCount', { n: formatInt(summary.totalFiles) })}
            </p>
          )}
        </div>
        <div className="results-actions">
          <button
            type="button"
            className={`btn-secondary${selectedFiles.size !== 1 ? ' is-idle' : ''}`}
            onClick={handlePreviewSelected}
            disabled={selectedFiles.size !== 1 || previewLoading || effectiveScanId <= 0 || !!scanBusy}
            title={scanBusy ? t('results.previewWhileBusy') : t('results.previewHint')}
          >
            <Eye size={16} /> {previewLoading ? t('results.previewing') : t('results.preview')}
          </button>
          <button type="button" className="btn-secondary" onClick={exportCsv} disabled={filteredFiles.length === 0 || csvExporting}>
            <Download size={16} /> {csvExporting ? t('results.exporting') : t('results.exportCsv')}
          </button>
          <label className="dup-toggle" title={t('results.preservePathsTitle')}>
            <input type="checkbox" checked={preservePaths} onChange={(e) => setPreservePaths(e.target.checked)} data-testid="preserve-paths" />
            {t('results.preservePaths')}
          </label>
          <button
            type="button"
            className={`btn-primary${selectedFiles.size === 0 ? ' is-idle' : ''}`}
            onClick={handleRecover}
            disabled={selectedFiles.size === 0 || isRecovering || !!scanBusy}
            title={scanBusy ? t('results.recoverBusyTitle') : undefined}
          >
            <ShieldCheck size={16} />
            {isRecovering ? t('results.recovering') : tFormat('results.recoverCount', { n: String(selectedFiles.size) })}
          </button>
        </div>
      </div>

      {recoverReport && (
        <div
          className={`glass-panel results-report ${!!recoverStats && (recoverStats.failed > 0 || recoverStats.zero > 0 || recoverStats.bad > 0) ? 'bad' : 'ok'}`}
          role={!!recoverStats && (recoverStats.failed > 0 || recoverStats.zero > 0 || recoverStats.bad > 0) ? 'alert' : 'status'}
        >
          {recoverReport}
        </div>
      )}
      {(preview || previewLoading) && (
        <ResultsPreviewPanel
          preview={preview}
          previewLoading={previewLoading}
          previewRecord={previewRecord}
          onClose={() => {
            // Invalidate any in-flight preview request — otherwise a slow
            // response re-opens the panel the user just closed (CA-028 gen).
            previewReqRef.current++
            setPreview(null)
            setPreviewTargetId(null)
            setPreviewLoading(false)
          }}
        />
      )}
      <HonestyBanners
        flags={honesty}
        loadFailed={honestyLoadFailed}
        ns="results"
        loadFailedTestId="results-honesty-load-error"
      />
      {listError && (
        <InlineAlert variant="error" testId="results-list-error" title={t('results.loadErrorTitle')}>
          {t('results.loadErrorBody')}
        </InlineAlert>
      )}
      {exportError && (
        <InlineAlert variant="error" onDismiss={() => setExportError(null)}>
          {exportError}
        </InlineAlert>
      )}
      {csvReport && (
        <div className="glass-panel results-report status-ok" role="status">
          {csvReport}
        </div>
      )}
      {hashReport && (
        <div className="glass-panel results-report status-ok" role="status" data-testid="content-hash-report">
          {hashReport}
        </div>
      )}

          {effectiveScanId > 0 && totalPages > 1 ? (
            <div className="pager" role="navigation" aria-label={t('results.pageLabel')}>
              <button type="button" className="btn-secondary" disabled={page === 0 || loading} onClick={() => setPage((p) => Math.max(0, p - 1))}>{t('results.prev')}</button>
              <span className="pager-status">
                {page * PAGE_SIZE + 1}–{Math.min((page + 1) * PAGE_SIZE, displayTotal)} / {formatInt(displayTotal)}
              </span>
              <label className="pager-jump">
                {t('results.pageLabel')}
                {/* Commit on blur/Enter only — an onChange per keystroke fired a
                    DB page load for every digit typed. key={page} resyncs the
                    draft after prev/next moves the page. */}
                <input
                  type="number"
                  min={1}
                  max={totalPages}
                  key={page}
                  defaultValue={page + 1}
                  onBlur={(e) => {
                    if (e.target.value === '') return
                    const n = Math.floor(Number(e.target.value))
                    if (!Number.isFinite(n)) return
                    setPage(Math.min(totalPages, Math.max(1, n)) - 1)
                  }}
                  onKeyDown={(e) => {
                    if (e.key === 'Enter') e.currentTarget.blur()
                  }}
                  aria-label={t('results.pageNumberAria')}
                />
                / {totalPages}
              </label>
              <button type="button" className="btn-secondary" disabled={page >= totalPages - 1 || loading} onClick={() => setPage((p) => p + 1)}>{t('results.next')}</button>
            </div>
          ) : null}

      <div className="results-content glass-panel">
        <div className="filters">
          <div className="filter-row">
            <span className="filter-label">{t('results.statusFilter')}</span>
            <button type="button" className="filter-chip" aria-pressed={statusFilter === 'deleted'} onClick={() => setStatusFilter('deleted')} data-testid="filter-deleted">{t('results.deleted')}</button>
            <button type="button" className="filter-chip" aria-pressed={statusFilter === 'allocated'} onClick={() => setStatusFilter('allocated')} data-testid="filter-allocated">{t('results.allocated')}</button>
            <button type="button" className="filter-chip" aria-pressed={statusFilter === 'carved'} onClick={() => setStatusFilter('carved')} data-testid="filter-carved">{t('results.carved')}</button>
            <button type="button" className="filter-chip" aria-pressed={statusFilter === 'all'} onClick={() => setStatusFilter('all')} data-testid="filter-all">{t('results.all')}</button>
            <label className="dup-toggle">
              <input type="checkbox" checked={showDuplicates} onChange={(e) => setShowDuplicates(e.target.checked)} data-testid="show-duplicates" />
              {t('results.duplicates')}
            </label>
            <button
              type="button"
              className="btn-secondary"
              data-testid="analyze-content-hash"
              onClick={() => void analyzeContentHash()}
              disabled={effectiveScanId <= 0 || hashingContent || !!scanBusy || !window.api?.hashEmptyContent}
              title={scanBusy ? t('results.analyzeDedupBusy') : undefined}
            >
              {hashingContent ? t('results.analyzingDedup') : t('results.analyzeDedup')}
            </button>
          </div>
          <div className="filter-row">
            <span className="filter-label">{t('results.typeFilter')}</span>
            <button type="button" className="filter-chip" aria-pressed={typeFilter === 'all'} onClick={() => setTypeFilter('all')}>{t('results.all')}</button>
            <button type="button" className="filter-chip" aria-pressed={typeFilter === 'img'} onClick={() => setTypeFilter('img')}>{t('scan.image')}</button>
            <button type="button" className="filter-chip" aria-pressed={typeFilter === 'video'} onClick={() => setTypeFilter('video')}>{t('scan.video')}</button>
            <button type="button" className="filter-chip" aria-pressed={typeFilter === 'audio'} onClick={() => setTypeFilter('audio')}>{t('scan.audio')}</button>
            <button type="button" className="filter-chip" aria-pressed={typeFilter === 'doc'} onClick={() => setTypeFilter('doc')}>{t('scan.document')}</button>
            <button type="button" className="filter-chip" aria-pressed={typeFilter === 'archive'} onClick={() => setTypeFilter('archive')}>{t('scan.archive')}</button>
            <input
              type="search"
              value={nameInput}
              onChange={(e) => setNameInput(e.target.value)}
              placeholder={t('results.searchByName')}
              aria-label={t('results.searchByNameAria')}
              className="name-search"
            />
            <button
              type="button"
              className="btn-secondary view-toggle is-end"
              title={viewMode !== 'flat' ? t('results.flatTitle') : t('results.galleryViewTitle')}
              onClick={() => setViewMode(viewMode === 'gallery' ? 'flat' : 'gallery')}
            >
              {viewMode === 'gallery' ? <List size={16} /> : <LayoutGrid size={16} />}
              {viewMode === 'gallery' ? t('results.list') : t('results.gallery')}
            </button>
            <button
              type="button"
              className="btn-secondary view-toggle"
              title={viewMode === 'tree' ? t('results.flatTitle') : t('results.treeViewTitle')}
              onClick={() => setViewMode(viewMode === 'tree' ? 'flat' : 'tree')}
            >
              {viewMode === 'tree' ? <List size={16} /> : <ListTree size={16} />}
              {viewMode === 'tree' ? t('results.list') : t('results.tree')}
            </button>
          </div>
          {/* P0-4: size/date filters — MB inputs and date range map to SQL bounds. */}
          <div className="filter-row metrics">
            <span className="filter-label">{t('results.sizeFilterLabel')}</span>
            <input
              type="number"
              min="0"
              step="0.1"
              value={sizeMinInput}
              onChange={(e) => setSizeMinInput(e.target.value)}
              placeholder={t('results.sizeMinPlaceholder')}
              aria-label={t('results.sizeMinAria')}
              className="name-search narrow"
            />
            <span aria-hidden="true">–</span>
            <input
              type="number"
              min="0"
              step="0.1"
              value={sizeMaxInput}
              onChange={(e) => setSizeMaxInput(e.target.value)}
              placeholder={t('results.sizeMaxPlaceholder')}
              aria-label={t('results.sizeMaxAria')}
              className="name-search narrow"
            />
            <span className="filter-label spaced">{t('results.dateFilterLabel')}</span>
            <input
              type="date"
              value={dateFromInput}
              onChange={(e) => setDateFromInput(e.target.value)}
              aria-label={t('results.dateFromAria')}
              className="date-input"
            />
            <span aria-hidden="true">–</span>
            <input
              type="date"
              value={dateToInput}
              onChange={(e) => setDateToInput(e.target.value)}
              aria-label={t('results.dateToAria')}
              className="date-input"
            />
            {(sizeMinInput || sizeMaxInput || dateFromInput || dateToInput) && (
              <button
                type="button"
                className="btn-secondary btn-compact"
                onClick={() => { setSizeMinInput(''); setSizeMaxInput(''); setDateFromInput(''); setDateToInput('') }}
              >
                {t('results.clearFilters')}
              </button>
            )}
          </div>
        </div>

        <div ref={scrollRef} onScroll={recomputeWindow} className={`results-scroll${viewMode === 'tree' ? ' tree-container' : ''}`} data-testid="results-scroll">
          {viewMode === 'tree' && totalCount > PAGE_SIZE && (
            <p className="tree-page-note">
              {tFormat('results.treePageNote', { n: String(filteredFiles.length), total: formatInt(totalCount) })}
            </p>
          )}
          {viewMode === 'gallery' ? (
            <div className="results-gallery">
              {galleryFiles.length === 0 ? (
                <div className="results-empty examiner-empty" role="status" data-testid="results-empty">
                  {galleryEmptyMessage}
                </div>
              ) : (
                <div className="results-gallery-grid">
                  {galleryFiles.map((f) => (
                    <ThumbCard
                      key={f.id}
                      f={f}
                      thumb={thumbs.get(f.id)}
                      thumbUrl={thumbUrls.get(f.id)}
                      onVisible={loadThumb}
                      onOpen={(id) => {
                        setSelectedFiles(new Set([id]))
                        void loadPreview(id)
                      }}
                      noPreviewLabel={t('results.noPreview')}
                      ariaLabel={tFormat('results.previewFileAria', { name: f.name })}
                    />
                  ))}
                </div>
              )}
            </div>
          ) : viewMode === 'tree' ? (
          <div className="results-tree-list">
            {filteredFiles.length === 0 ? (
              <div className="results-empty examiner-empty" role="status" data-testid="results-empty">
                {listEmptyMessage}
              </div>
            ) : renderTreeNode(treeRoot, 0)}
          </div>
          ) : (
          <table className="results-table">
            <thead>
              <tr>
                <th className="check">
                  <input type="checkbox" checked={allPageSelected} onChange={toggleAll} aria-label={t('results.selectAllPage')} />
                </th>
                {([
                  [t('results.col.name'), 'name'],
                  [t('results.col.path'), 'path'],
                ] as const).map(([label, field]) => (
                  <th key={field} {...sortableTh(field)} className="sortable">
                    {label}
                    <span aria-hidden="true">{sortIndicator(field)}</span>
                  </th>
                ))}
                {([
                  [t('results.col.size'), 'size'],
                  [t('results.col.date'), 'date'],
                ] as const).map(([label, field]) => (
                  <th key={field} {...sortableTh(field)} className="sortable">
                    {label}
                    <span aria-hidden="true">{sortIndicator(field)}</span>
                  </th>
                ))}
                <th {...sortableTh('confidence')} className="sortable">
                  {t('results.col.confidence')}<span aria-hidden="true">{sortIndicator('confidence')}</span>
                </th>
                <th>{t('results.col.source')}</th>
                <th>{t('results.col.status')}</th>
              </tr>
            </thead>
            <tbody>
              {filteredFiles.length === 0 ? (
                <tr>
                  <td colSpan={8} className="results-empty-cell">
                    <div className="examiner-empty" role="status" data-testid="results-empty">
                      {listEmptyMessage}
                    </div>
                  </td>
                </tr>
              ) : (
                <>
                  {/* FAZ 1.3a: spacer rows stand in for the unrendered window
                      neighbours so the scrollbar geometry stays honest; the
                      data-testid="result-row" contract is unchanged. */}
                  {vWindow.start > 0 && (
                    <tr aria-hidden="true" style={{ height: vWindow.start * rowHeight }}>
                      <td colSpan={8} className="spacer-cell" />
                    </tr>
                  )}
                  {visibleFiles.map((f, idx) => {
                  const titleParts = [f.path !== '—' ? tFormat('results.locationPrefix', { path: f.path }) : null, f.qualityLabel !== '—' ? tFormat('results.qualityPrefix', { q: f.qualityLabel }) : null]
                    .filter(Boolean)
                    .join(' · ')
                  const statusClass =
                    f.statusKey === 'status.deleted'
                      ? 'status-deleted'
                      : f.statusKey === 'status.carved'
                        ? 'status-carved'
                        : 'status-allocated'
                  return (
                  <tr
                    key={f.id}
                    ref={idx === 0 ? rowProbeRef : undefined}
                    data-testid="result-row"
                    title={titleParts || undefined}
                    tabIndex={0}
                    onKeyDown={(e) => {
                      if (e.key === 'Enter') {
                        setSelectedFiles(new Set([f.id]))
                        void loadPreview(f.id)
                      }
                    }}
                    onDoubleClick={() => {
                      setSelectedFiles(new Set([f.id]))
                      void loadPreview(f.id)
                    }}
                    className={selectedFiles.has(f.id) ? 'is-selected' : undefined}
                  >
                    <td>
                      <input type="checkbox" checked={selectedFiles.has(f.id)} onChange={() => toggleSelection(f.id)} aria-label={tFormat('results.selectFile', { name: f.name })} />
                    </td>
                    <td className="file-name-cell">
                      {getIconForType(f.type)}
                      {f.name}
                      {typeof f.mftRef === 'number' && onShowMft ? (
                        <button
                          type="button"
                          className="btn-secondary show-mft"
                          data-testid="show-mft"
                          title={t('results.showMft')}
                          onClick={(e) => {
                            e.stopPropagation()
                            onShowMft(f.mftRef as number)
                          }}
                        >
                          <Binary size={14} aria-hidden="true" />
                          {t('results.showMft')}
                        </button>
                      ) : null}
                      {f.sameContentGroup >= 2 ? (
                        <span
                          className="same-content-badge"
                          data-testid="same-content-badge"
                          title={tFormat('results.sameContentTitle', { n: String(f.sameContentGroup) })}
                        >
                          {t('results.sameContent')}
                        </span>
                      ) : null}
                    </td>
                    <td
                      className="path-cell"
                      title={f.path !== '—' ? f.path : undefined}
                    >
                      {f.path}
                    </td>
                    <td className="cell-muted">{f.size}</td>
                    <td className="cell-date">{f.dateLabel}</td>
                    <td>
                      {f.confidenceTier === 'none' ? (
                        <span className="confidence-badge tier-none">—</span>
                      ) : (
                        <span
                          data-testid={`confidence-${f.confidence ?? 0}`}
                          className={`confidence-badge tier-${f.confidenceTier}`}
                          title={f.qualityLabel}
                        >
                          {f.confidence ?? 0}
                        </span>
                      )}
                    </td>
                    <td className="cell-source">{f.sourceLabel}</td>
                    <td>
                      <span className={`status-badge ${statusClass}`}>
                        {f.status}
                      </span>
                    </td>
                  </tr>
                  )
                  })}
                  {vWindow.end < filteredFiles.length && (
                    <tr aria-hidden="true" style={{ height: (filteredFiles.length - vWindow.end) * rowHeight }}>
                      <td colSpan={8} className="spacer-cell" />
                    </tr>
                  )}
                </>
              )}
            </tbody>
          </table>
          )}
        </div>
      </div>
    </div>
  )
}

export default ResultsView
