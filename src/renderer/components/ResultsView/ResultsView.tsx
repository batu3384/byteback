import React, { useState, useEffect, useCallback, useRef, useMemo } from 'react'
import './ResultsView.css'
import { File, FileImage, FileText, FileVideo, FileAudio, FileArchive, Download, ShieldCheck, Folder, FolderOpen, ListTree, List, Eye, LayoutGrid, Loader2 } from 'lucide-react'
import type { FileRecord, FilePreviewResult, RaidState } from '../../../shared/ipc-contract'
import { sourceDisplayLabel, isDiscoveryOnlySource, canRecoverSource, isRecoverableListSource, isDuplicateSource } from '../../../shared/source-label'
import { csvCell } from '../../../shared/html-escape'
import { diskBusyMessage } from '../../../shared/scan-required'
import { isDestOnScannedDrive, isDestOnRaidMemberDrive } from '../../../shared/recover-dest-guard'
import { previewDataUrl } from '../../../shared/preview-utils'
import ResultsPreviewPanel from './ResultsPreviewPanel'
import {
  qualityHint,
  resolveFileTypeChip,
  formatSize,
  formatFsTimestamp,
  statusDisplayLabel,
  buildTree,
  toSqlListFilter,
  confidenceTier,
  sortKey,
  type MappedFile,
  type TreeNode,
  type SortField,
  type SortDir,
} from './results-view-utils'

const INACTIVE_RAID: RaidState = { active: false, capacity: 0, numDisks: 0, level: -1, memberDriveIndices: [] }

interface ResultsViewProps {
  filesFound: any[]
  driveIndex: number | null
  scanId?: number
  scanBusy?: boolean
}

const PAGE_SIZE = 500

function ResultsView({ filesFound, driveIndex, scanId, scanBusy }: ResultsViewProps): React.ReactElement {
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
  const [recordById, setRecordById] = useState<Map<number, FileRecord>>(new Map())
  const [hfsTruncated, setHfsTruncated] = useState(false)
  const [recoverReport, setRecoverReport] = useState<string | null>(null)
  const [preview, setPreview] = useState<FilePreviewResult | null>(null)
  const [previewLoading, setPreviewLoading] = useState(false)
  const [previewTargetId, setPreviewTargetId] = useState<number | null>(null)
  const [sortField, setSortField] = useState<SortField>('confidence')
  const [sortDir, setSortDir] = useState<SortDir>('desc')
  const [thumbs, setThumbs] = useState<Map<number, FilePreviewResult>>(new Map())
  const thumbsRef = useRef(thumbs)
  thumbsRef.current = thumbs
  const thumbLoadingRef = useRef<Set<number>>(new Set())
  const previewReqRef = useRef(0)
  const [csvExporting, setCsvExporting] = useState(false)
  const loadGenRef = useRef(0)

  const effectiveScanId = scanId && scanId > 0 ? scanId : -1

  const loadPreview = async (fileId: number) => {
    if (effectiveScanId <= 0 || !window.api?.readFilePreview) {
      setPreview({ success: false, error: 'Önizleme için tarama kimliği gerekli.' })
      return
    }
    const raidState = window.api?.getRaidState ? await window.api.getRaidState() : INACTIVE_RAID
    const effectiveDrive = driveIndex !== null ? driveIndex : -1
    if (effectiveDrive < 0 && !raidState.active) {
      setPreview({ success: false, error: 'Önizleme için sürücü veya RAID gerekli.' })
      return
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
      setPreview({ success: false, error: diskBusyMessage(raw) ?? 'Önizleme okunamadı.' })
    } finally {
      if (gen === previewReqRef.current) setPreviewLoading(false)
    }
  }

  const loadPage = useCallback(async (scan: number, pageIndex: number) => {
    if (!window.api?.getFilesPage || !window.api?.getFileCount || scan <= 0) return
    const gen = ++loadGenRef.current
    setLoading(true)
    const listFilter = toSqlListFilter(statusFilter, typeFilter, nameQuery, showDuplicates, sortKey(sortField, sortDir))
    try {
      const [count, pageData, sum] = await Promise.all([
        window.api.getFileCount(scan, listFilter),
        window.api.getFilesPage(scan, pageIndex * PAGE_SIZE, PAGE_SIZE, listFilter),
        window.api.getScanSummary ? window.api.getScanSummary(scan) : Promise.resolve(null),
      ])
      if (gen !== loadGenRef.current) return
      setTotalCount(typeof count === 'number' && count >= 0 ? count : 0)
      setDbFiles(pageData ?? [])
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
      setDbFiles([])
      setTotalCount(0)
    } finally {
      if (gen === loadGenRef.current) setLoading(false)
    }
  }, [statusFilter, typeFilter, nameQuery, showDuplicates, sortField, sortDir])

  useEffect(() => {
    const t = setTimeout(() => setNameQuery(nameInput.trim()), 300)
    return () => clearTimeout(t)
  }, [nameInput])

  useEffect(() => {
    setPage(0)
    setSelectedFiles(new Set())
  }, [statusFilter, typeFilter, nameQuery, showDuplicates])

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
    const localHit = (effectiveScanId > 0 ? dbFiles : filesFound).some(
      (f: { source?: string }) => f.source === 'hfs_limit',
    )
    if (localHit) {
      setHfsTruncated(true)
      return
    }
    if (effectiveScanId <= 0 || !window.api?.searchFiles) {
      setHfsTruncated(false)
      return
    }
    void window.api
      .searchFiles(effectiveScanId, 'catalog truncated', 0, 8)
      .then((res) => setHfsTruncated(res.rows.some((r: { source?: string }) => r.source === 'hfs_limit')))
      .catch(() => setHfsTruncated(false))
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
      })).filter((f) => isRecoverableListSource(f.source) || (showDuplicates && isDuplicateSource(f.source)))

  const handleRecover = async () => {
    if (scanBusy) {
      setRecoverReport('Tarama sürerken kurtarma yapılamaz. Tarama bitince tekrar dene.')
      return
    }
    if (effectiveScanId <= 0) {
      setRecoverReport('Kurtarma yalnız tarama veritabanındaki kayıtlardan yapılır. Tarama bitsin, sonra sonuç listesinden seçin.')
      return
    }
    if (selectedFiles.size === 0) return
    const raidState = window.api?.getRaidState ? await window.api.getRaidState() : INACTIVE_RAID
    const effectiveDrive = driveIndex !== null ? driveIndex : -1
    if (effectiveDrive < 0 && !raidState.active) {
      setRecoverReport('Kurtarma için bir sürücü veya aktif RAID dizisi gerekli.')
      return
    }
    if (!window.api?.recoverFile) {
      setRecoverReport('Kurtarma API\'si kullanılamıyor.')
      return
    }

    setIsRecovering(true)
    setRecoverReport(null)
    try {
      let destDir = await window.api.pickDirectory()
      if (!destDir) return
    if (
      effectiveDrive >= 0 &&
      window.api.resolveVolume &&
      (await isDestOnScannedDrive(destDir, effectiveDrive, (letter) => window.api.resolveVolume(letter)))
    ) {
      const proceed = window.confirm(
        'Hedef klasör taradığın fiziksel sürücüde. Kurtarma silinen verinin üzerine yazabilir.\n\nBaşka bir disk seçmen önerilir. Yine de devam edilsin mi?',
      )
      if (!proceed) return
    } else if (
      raidState.active &&
      window.api.resolveVolume &&
      (await isDestOnRaidMemberDrive(
        destDir,
        raidState.memberDriveIndices ?? [],
        (letter) => window.api.resolveVolume(letter),
      ))
    ) {
      const proceed = window.confirm(
        'Hedef klasör RAID dizisinin üye disklerinden birinde. Kurtarma silinen verinin üzerine yazabilir.\n\nBaşka bir disk seçmen önerilir. Yine de devam edilsin mi?',
      )
      if (!proceed) return
    }

    const filesToRecover: FileRecord[] = []
    const skipped: string[] = []
    for (const id of selectedFiles) {
      const fileToRecover = recordById.get(id) ?? sourceFiles.find(f => f.id === id)
      if (!fileToRecover) continue
      const hasRuns = (fileToRecover.runs?.length ?? 0) > 0
      if (!canRecoverSource(fileToRecover.source, hasRuns) || isDiscoveryOnlySource(fileToRecover.source)) {
        skipped.push(`${fileToRecover.name} (${sourceDisplayLabel(fileToRecover.source)})`)
        continue
      }
      filesToRecover.push(fileToRecover)
    }
    const fileIds = filesToRecover.map((f) => f.id).filter((id) => id > 0)
    if (fileIds.length === 0) {
      setRecoverReport(
        skipped.length
          ? 'Seçilen kayıtlar yalnızca keşif veya SQLite kimliği yok:\n' + skipped.join('\n')
          : 'Seçilen kayıtların SQLite kimliği yok.',
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

    const noteResult = async (res: { success?: boolean; zeroFilled?: boolean; error?: string; validationScore?: number; validationError?: string; md5Hash?: string; destPath?: string }, id: number) => {
      if (res.success) successCount++
      else {
        failedCount++
        if (res.error) errors.push(`#${id}: ${res.error}`)
      }
      if (res.zeroFilled) zeroFilledCount++
      if (res.validationError) errors.push(`#${id} doğrulama: ${res.validationError}`)
      if (res.md5Hash) {
        let nsrlLine = `#${id} MD5: ${res.md5Hash}`
        if (window.api.lookupNsrl) {
          try {
            const known = await window.api.lookupNsrl(res.md5Hash)
            if (known) nsrlLine += ' (NSRL: bilinen hash)'
          } catch {
            /* NSRL lookup optional */
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
        errors.push(diskBusyMessage(raw) ?? 'Toplu kurtarma istisnası')
      }
    } else {
      for (const fileId of fileIds) {
        try {
          const res = await window.api.recoverFile(
            effectiveDrive,
            fileId,
            destDir,
            effectiveScanId,
          )
          await noteResult(res, fileId)
        } catch {
          failedCount++
          errors.push(`#${fileId}: istisna`)
        }
      }
    }

    setIsRecovering(false)
    const skipLine = skipped.length ? `\nAtlanan keşif kaydı: ${skipped.length}` : ''
    const padWarn =
      zeroFilledCount > 0
        ? `\nUyarı: ${zeroFilledCount} dosya eksik/sıfır-pad okuma ile yazıldı — içerik eksik olabilir, önizlemeyi kontrol et.`
        : ''
    const validationLine =
      validatedOk + validatedBad > 0
        ? `\nDoğrulama (carve): Tam ${validatedOk}, Bozuk ${validatedBad}${
            validatedBad > 0 ? ' — bozuk skorlu dosyalar kurtarma başarısız sayılır' : ''
          }`
        : ''
    const verifiedLine = verified.length ? `\nDoğrulananlar:\n${verified.slice(0, 8).join('\n')}` : ''
    const errLine = errors.length ? `\nHatalar:\n${errors.slice(0, 8).join('\n')}` : ''
    setRecoverReport(
      `Kurtarma bitti. Başarılı: ${successCount}. Başarısız: ${failedCount}. Eksik/pad okuma: ${zeroFilledCount}. Hedef: ${destDir}${skipLine}${padWarn}${validationLine}${verifiedLine}${errLine}`,
    )
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
    if (effectiveScanId <= 0 || !window.api?.getFilesPage || !window.api?.getFileCount || csvExporting) return
    const listFilter = toSqlListFilter(statusFilter, typeFilter, nameQuery, showDuplicates, sortKey(sortField, sortDir))
    setCsvExporting(true)
    try {
      const total = await window.api.getFileCount(effectiveScanId, listFilter)
      // CA-038: build the CSV per batch and hand Blob the chunk array — no
      // multi-hundred-MB string concat of every record in renderer memory.
      const batch = 1000
      const header = ['name', 'sizeBytes', 'category', 'confidence', 'status', 'path', 'source', 'startSector', 'createdAt', 'modifiedAt']
      const chunks: string[] = [header.join(';')]
      for (let off = 0; off < total; off += batch) {
        const chunk = await window.api.getFilesPage(effectiveScanId, off, batch, listFilter)
        for (const raw of chunk ?? []) {
          const row = [
            raw.name,
            raw.sizeBytes ?? '',
            raw.category ?? '',
            raw.confidence ?? '',
            raw.status ?? '',
            raw.path ?? '',
            raw.source ?? '',
            raw.startSector ?? '',
            raw.createdAt && raw.createdAt > 0 ? new Date(raw.createdAt * 1000).toISOString() : formatFsTimestamp(0, raw.source),
            raw.modifiedAt && raw.modifiedAt > 0 ? new Date(raw.modifiedAt * 1000).toISOString() : formatFsTimestamp(0, raw.source),
          ]
          chunks.push(row.map((v) => csvCell(v)).join(';'))
        }
        if ((chunk?.length ?? 0) < batch) break
      }
      const blob = new Blob(['\uFEFF' + chunks.join('\r\n')], { type: 'text/csv;charset=utf-8' })
      const url = URL.createObjectURL(blob)
      const a = document.createElement('a')
      a.href = url
      a.download = `byteback-sonuclar-${total}-kayit-${new Date().toISOString().slice(0, 10)}.csv`
      a.click()
      URL.revokeObjectURL(url)
    } catch {
      window.alert('CSV dışa aktarım başarısız.')
    } finally {
      setCsvExporting(false)
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
    type: resolveFileTypeChip(f),
    sourceLabel: sourceDisplayLabel(f.source),
    dateLabel: formatFsTimestamp(f.modifiedAt || f.createdAt, f.source),
    qualityLabel: qualityHint(f),
    confidence: f.confidence,
    confidenceTier: confidenceTier(f.confidence),
  }))

  const filteredFiles = mappedFiles
  const treeRoot = buildTree(filteredFiles)
  const galleryFiles = filteredFiles.filter((f) => f.type === 'img')

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
      setSortDir(field === 'name' ? 'asc' : 'desc')
    }
    setPage(0)
  }

  const sortIndicator = (field: SortField) => (sortField === field ? (sortDir === 'asc' ? ' ▲' : ' ▼') : '')

  const loadThumb = useCallback(async (id: number) => {
    if (thumbsRef.current.has(id) || thumbLoadingRef.current.has(id)) return
    const raidState = window.api?.getRaidState ? await window.api.getRaidState() : INACTIVE_RAID
    const effectiveDrive = driveIndex !== null ? driveIndex : -1
    if (effectiveDrive < 0 && !raidState.active) return
    if (effectiveScanId <= 0 || !window.api?.readFilePreview) return
    thumbLoadingRef.current.add(id)
    try {
      const res = await window.api.readFilePreview(effectiveDrive, effectiveScanId, id)
      setThumbs((prev) => {
        const next = new Map(prev)
        next.set(id, res)
        return next
      })
    } catch {
      /* thumbnail is best-effort */
    } finally {
      thumbLoadingRef.current.delete(id)
    }
  }, [driveIndex, effectiveScanId])

  // Gallery thumbnail card: lazy-loads its 64KB preview when scrolled into view.
  const ThumbCard = ({ f }: { f: MappedFile }): React.ReactElement => {
    const [visible, setVisible] = useState(false)
    const imgRef = useRef<HTMLDivElement | null>(null)
    useEffect(() => {
      const el = imgRef.current
      if (!el || visible) return
      const io = new IntersectionObserver(
        (entries) => {
          if (entries.some((en) => en.isIntersecting)) {
            setVisible(true)
            void loadThumb(f.id)
          }
        },
        { rootMargin: '200px' },
      )
      io.observe(el)
      return () => io.disconnect()
    }, [visible, f.id, loadThumb])
    const thumb = thumbs.get(f.id)
    const dataUrl = useMemo(() => (thumb ? previewDataUrl(thumb) : null), [thumb])
    return (
      <div
        ref={imgRef}
        style={{ border: '1px solid var(--panel-border)', borderRadius: '8px', overflow: 'hidden', background: 'rgba(255,255,255,0.02)', cursor: 'pointer' }}
        onClick={() => {
          setSelectedFiles(new Set([f.id]))
          void loadPreview(f.id)
        }}
        role="button"
        tabIndex={0}
        onKeyDown={(e) => {
          if (e.key === 'Enter' || e.key === ' ') {
            setSelectedFiles(new Set([f.id]))
            void loadPreview(f.id)
          }
        }}
        aria-label={`${f.name} önizleme`}
      >
        <div style={{ aspectRatio: '1', display: 'flex', alignItems: 'center', justifyContent: 'center', background: 'rgba(0,0,0,0.25)' }}>
          {dataUrl ? (
            <img src={dataUrl} alt={f.name} style={{ width: '100%', height: '100%', objectFit: 'cover' }} loading="lazy" />
          ) : thumb ? (
            <span style={{ fontSize: '0.75rem', color: 'var(--text-muted)', padding: '8px', textAlign: 'center' }}>önizlenemedi</span>
          ) : (
            <Loader2 size={20} className="spinner" color="var(--text-muted)" />
          )}
        </div>
        <div style={{ padding: '6px 8px', fontSize: '0.72rem', color: 'var(--text-muted)', overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap', fontFamily: 'monospace' }} title={f.name}>
          {f.name}
        </div>
      </div>
    )
  }

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
          style={{ display: 'flex', alignItems: 'center', gap: '8px', padding: '6px 12px', cursor: 'pointer', marginLeft: depth * 16, borderRadius: '4px', width: 'calc(100% - ' + (depth * 16) + 'px)', background: 'transparent', border: 'none', color: 'inherit', textAlign: 'left' }}
          onMouseEnter={(e) => (e.currentTarget.style.background = 'rgba(255,255,255,0.03)')}
          onMouseLeave={(e) => (e.currentTarget.style.background = 'transparent')}
        >
          {isOpen ? <FolderOpen size={16} color="var(--accent-blue)" /> : <Folder size={16} color="var(--accent-blue)" />}
          <span style={{ fontWeight: 500 }}>{dir.name}</span>
          <span style={{ fontSize: '0.75rem', color: 'var(--text-muted)' }}>{childCount} öğe</span>
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
          style={{ display: 'flex', alignItems: 'center', gap: '8px', padding: '6px 12px', marginLeft: (depth + 1) * 16, cursor: 'pointer', borderRadius: '4px', background: selectedFiles.has(f.id) ? 'rgba(59, 130, 246, 0.1)' : 'transparent', border: 'none', color: 'inherit', textAlign: 'left', width: 'calc(100% - ' + ((depth + 1) * 16) + 'px)' }}
        >
          <input type="checkbox" checked={selectedFiles.has(f.id)} onChange={() => toggleSelection(f.id)} onClick={(e) => e.stopPropagation()} style={{ width: 14, height: 14 }} aria-hidden="true" tabIndex={-1} />
          {getIconForType(f.type)}
          <span style={{ fontFamily: 'monospace', overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }}>{f.name}</span>
          {f.sourceLabel ? <span style={{ color: 'var(--text-muted)', fontSize: '0.75rem', flexShrink: 0 }}>{f.sourceLabel}</span> : null}
          <span style={{ marginLeft: 'auto', color: 'var(--text-muted)', fontSize: '0.8rem', flexShrink: 0 }}>{f.size}</span>
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
    <div className="results-view" style={{ display: 'flex', flexDirection: 'column', gap: 'var(--space-lg)', height: '100%' }}>
      <div className="results-header glass-panel" style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center', padding: '24px' }}>
        <div className="results-info">
          <h2 style={{ fontSize: '1.5rem', marginBottom: '4px' }}>Kurtarma Sonuçları</h2>
          <p style={{ color: 'var(--text-muted)' }}>
            Bu süzgeçte {displayTotal.toLocaleString('tr-TR')} dosya
            {effectiveScanId > 0 && totalPages > 1 ? ` — sayfa ${page + 1}/${totalPages}` : ''}
            {loading ? ' (yükleniyor...)' : ''}
          </p>
          {effectiveScanId > 0 && (
            <p style={{ color: 'var(--text-muted)', fontSize: '0.85rem', marginTop: '4px' }}>
              Silinmiş {summary.deletedFiles.toLocaleString('tr-TR')}
              {' · '}Tahsisli {Math.max(0, summary.totalFiles - summary.deletedFiles - (summary.carvedFiles ?? 0)).toLocaleString('tr-TR')}
              {' · '}Oyulmuş {(summary.carvedFiles ?? 0).toLocaleString('tr-TR')}
              {' · '}Toplam {summary.totalFiles.toLocaleString('tr-TR')}
            </p>
          )}
        </div>
        <div className="results-actions" style={{ display: 'flex', gap: '12px' }}>
          <button
            className="btn-secondary"
            style={{ display: 'flex', gap: '8px', opacity: selectedFiles.size !== 1 ? 0.5 : 1 }}
            onClick={handlePreviewSelected}
            disabled={selectedFiles.size !== 1 || previewLoading || effectiveScanId <= 0}
            title="Tek dosya seçiliyken ilk 64 KB önizleme"
          >
            <Eye size={16} /> {previewLoading ? 'Önizleniyor...' : 'Önizle'}
          </button>
          <button className="btn-secondary" style={{ display: 'flex', gap: '8px' }} onClick={exportCsv} disabled={filteredFiles.length === 0 || csvExporting}>
            <Download size={16} /> {csvExporting ? 'Aktarılıyor…' : 'Dışa Aktar (CSV)'}
          </button>
          <button 
            className="btn-primary" 
            style={{ display: 'flex', gap: '8px', opacity: selectedFiles.size === 0 ? 0.5 : 1, cursor: selectedFiles.size === 0 ? 'not-allowed' : 'pointer' }}
            onClick={handleRecover}
            disabled={selectedFiles.size === 0 || isRecovering || !!scanBusy}
            title={scanBusy ? 'Tarama bitene kadar kurtarma kapalı' : undefined}
          >
            <ShieldCheck size={16} /> 
            {isRecovering ? 'Kurtarılıyor...' : `Seçilenleri Kurtar (${selectedFiles.size})`}
          </button>
        </div>
      </div>

      {recoverReport && (
        <div
          className="glass-panel"
          role={/Başarısız: [1-9]|Uyarı:|Bozuk [1-9]/.test(recoverReport) ? 'alert' : 'status'}
          style={{
            padding: '16px 24px',
            borderLeft: `4px solid ${
              /Başarısız: [1-9]|Uyarı:|Bozuk [1-9]/.test(recoverReport)
                ? 'var(--alert-red)'
                : 'var(--accent-blue)'
            }`,
            whiteSpace: 'pre-wrap',
          }}
        >
          {recoverReport}
        </div>
      )}
      {(preview || previewLoading) && (
        <ResultsPreviewPanel
          preview={preview}
          previewLoading={previewLoading}
          previewRecord={previewRecord}
          onClose={() => { setPreview(null); setPreviewTargetId(null) }}
        />
      )}
      {hfsTruncated && (
        <div className="glass-panel" role="alert" style={{ padding: '16px 24px', borderLeft: '4px solid var(--warning-yellow)' }}>
          HFS+ katalog bu taramada limit sentinel kaydı üretti. Varsayılan tarama sınırsızdır; bu satır yalnız limit verilmişse görünür.
        </div>
      )}

          {effectiveScanId > 0 && totalPages > 1 ? (
            <div className="pager" role="navigation" aria-label="Sayfa">
              <button type="button" className="btn-secondary" disabled={page === 0 || loading} onClick={() => setPage((p) => Math.max(0, p - 1))}>Önceki</button>
              <span className="pager-status">
                {page * PAGE_SIZE + 1}–{Math.min((page + 1) * PAGE_SIZE, displayTotal)} / {displayTotal.toLocaleString('tr-TR')}
              </span>
              <label className="pager-jump">
                Sayfa
                <input
                  type="number"
                  min={1}
                  max={totalPages}
                  value={page + 1}
                  onChange={(e) => {
                    const n = Number(e.target.value)
                    if (!Number.isFinite(n)) return
                    setPage(Math.min(totalPages, Math.max(1, Math.floor(n))) - 1)
                  }}
                  aria-label="Sayfa numarası"
                />
                / {totalPages}
              </label>
              <button type="button" className="btn-secondary" disabled={page >= totalPages - 1 || loading} onClick={() => setPage((p) => p + 1)}>Sonraki</button>
            </div>
          ) : null}

      <div className="results-content glass-panel" style={{ flex: 1, display: 'flex', flexDirection: 'column', overflow: 'hidden' }}>
        <div className="filters">
          <div className="filter-row">
            <span className="filter-label">Durum</span>
            <button type="button" className="filter-chip" aria-pressed={statusFilter === 'deleted'} onClick={() => setStatusFilter('deleted')} data-testid="filter-deleted">Silinmiş</button>
            <button type="button" className="filter-chip" aria-pressed={statusFilter === 'allocated'} onClick={() => setStatusFilter('allocated')}>Tahsisli</button>
            <button type="button" className="filter-chip" aria-pressed={statusFilter === 'carved'} onClick={() => setStatusFilter('carved')} data-testid="filter-carved">Oyulmuş</button>
            <button type="button" className="filter-chip" aria-pressed={statusFilter === 'all'} onClick={() => setStatusFilter('all')}>Tümü</button>
            <label className="dup-toggle">
              <input type="checkbox" checked={showDuplicates} onChange={(e) => setShowDuplicates(e.target.checked)} data-testid="show-duplicates" />
              Tekrarlar
            </label>
          </div>
          <div className="filter-row">
            <span className="filter-label">Tip</span>
            <button type="button" className="filter-chip" aria-pressed={typeFilter === 'all'} onClick={() => setTypeFilter('all')}>Hepsi</button>
            <button type="button" className="filter-chip" aria-pressed={typeFilter === 'img'} onClick={() => setTypeFilter('img')}>Resim</button>
            <button type="button" className="filter-chip" aria-pressed={typeFilter === 'video'} onClick={() => setTypeFilter('video')}>Video</button>
            <button type="button" className="filter-chip" aria-pressed={typeFilter === 'audio'} onClick={() => setTypeFilter('audio')}>Ses</button>
            <button type="button" className="filter-chip" aria-pressed={typeFilter === 'doc'} onClick={() => setTypeFilter('doc')}>Belge</button>
            <button type="button" className="filter-chip" aria-pressed={typeFilter === 'archive'} onClick={() => setTypeFilter('archive')}>Arşiv</button>
            <input
              type="search"
              value={nameInput}
              onChange={(e) => setNameInput(e.target.value)}
              placeholder="Ada göre ara"
              aria-label="Dosya adı ara"
              className="name-search"
            />
            <button
              type="button"
              className="btn-secondary"
              title={viewMode === 'tree' ? 'Düz liste' : viewMode === 'gallery' ? 'Düz liste' : 'Resim galerisi'}
              onClick={() => setViewMode(viewMode === 'gallery' ? 'flat' : 'gallery')}
              style={{ padding: '6px 12px', display: 'flex', gap: '6px', alignItems: 'center', marginLeft: 'auto' }}
            >
              {viewMode === 'gallery' ? <List size={16} /> : <LayoutGrid size={16} />}
              {viewMode === 'gallery' ? 'Liste' : 'Galeri'}
            </button>
            <button
              type="button"
              className="btn-secondary"
              title={viewMode === 'tree' ? 'Düz liste' : 'Dizin ağacı'}
              onClick={() => setViewMode(viewMode === 'tree' ? 'flat' : 'tree')}
              style={{ padding: '6px 12px', display: 'flex', gap: '6px', alignItems: 'center' }}
            >
              {viewMode === 'tree' ? <List size={16} /> : <ListTree size={16} />}
              {viewMode === 'tree' ? 'Liste' : 'Ağaç'}
            </button>
          </div>
        </div>

        <div style={{ flex: 1, overflowY: 'auto', padding: '0 24px' }} className={viewMode === 'tree' ? 'tree-container' : ''}>
          {viewMode === 'tree' && totalCount > PAGE_SIZE && (
            <p style={{ padding: '8px 0', color: 'var(--warning-yellow)', fontSize: '0.85rem' }}>
              Ağaç görünümü yalnızca bu sayfadaki {filteredFiles.length} kaydı gösterir ({totalCount.toLocaleString('tr-TR')} toplam). Tam liste için düz görünüm veya CSV dışa aktarım kullanın.
            </p>
          )}
          {viewMode === 'gallery' ? (
            <div style={{ padding: '16px 0' }}>
              {galleryFiles.length === 0 ? (
                <div style={{ textAlign: 'center', padding: '3rem', color: 'var(--text-muted)' }}>
                  {loading ? 'Yükleniyor…' : 'Bu sayfada resim yok. Galeri yalnızca geçerli sayfadaki resimleri gösterir.'}
                </div>
              ) : (
                <div style={{ display: 'grid', gridTemplateColumns: 'repeat(auto-fill, minmax(160px, 1fr))', gap: '12px' }}>
                  {galleryFiles.map((f) => (
                    <ThumbCard key={f.id} f={f} />
                  ))}
                </div>
              )}
            </div>
          ) : viewMode === 'tree' ? (
          <div style={{ padding: '12px 0', display: 'flex', flexDirection: 'column', gap: '2px' }}>
            {filteredFiles.length === 0 ? (
              <div style={{ textAlign: 'center', padding: '3rem', color: 'var(--text-muted)' }}>
                {loading ? 'Yükleniyor…' : 'Bu süzgeçte dosya yok.'}
              </div>
            ) : renderTreeNode(treeRoot, 0)}
          </div>
          ) : (
          <table className="results-table" style={{ width: '100%', borderCollapse: 'collapse', textAlign: 'left' }}>
            <thead style={{ position: 'sticky', top: 0, background: 'var(--bg-surface)', zIndex: 1 }}>
              <tr>
                <th style={{ padding: '12px', borderBottom: '1px solid var(--panel-border)', width: '40px' }}>
                  <input type="checkbox" checked={allPageSelected} onChange={toggleAll} aria-label="Bu sayfadaki tümünü seç" />
                </th>
                {([
                  ['Dosya Adı', 'name'],
                  ['Boyut', 'size'],
                  ['Değiştirilme', 'date'],
                ] as const).map(([label, field]) => (
                  <th key={field} style={{ padding: '12px', borderBottom: '1px solid var(--panel-border)', cursor: 'pointer', userSelect: 'none' }} onClick={() => toggleSort(field)}>
                    {label}
                    <span aria-hidden="true">{sortIndicator(field)}</span>
                  </th>
                ))}
                <th style={{ padding: '12px', borderBottom: '1px solid var(--panel-border)', cursor: 'pointer', userSelect: 'none' }} onClick={() => toggleSort('confidence')}>
                  Güven<span aria-hidden="true">{sortIndicator('confidence')}</span>
                </th>
                <th style={{ padding: '12px', borderBottom: '1px solid var(--panel-border)' }}>Kaynak</th>
                <th style={{ padding: '12px', borderBottom: '1px solid var(--panel-border)' }}>Durum</th>
              </tr>
            </thead>
            <tbody>
              {filteredFiles.length === 0 ? (
                <tr>
                  <td colSpan={7} style={{ textAlign: 'center', padding: '3rem', color: 'var(--text-muted)' }}>
                    {loading ? 'Yükleniyor…' : 'Bu süzgeçte dosya yok.'}
                  </td>
                </tr>
              ) : (
                filteredFiles.map((f) => {
                  const titleParts = [f.path !== '—' ? `Konum: ${f.path}` : null, f.qualityLabel !== '—' ? `Kalite: ${f.qualityLabel}` : null]
                    .filter(Boolean)
                    .join(' · ')
                  const tierColor = f.confidenceTier === 'high' ? 'var(--success-green)' : f.confidenceTier === 'mid' ? 'var(--warning-yellow)' : 'var(--alert-red)'
                  return (
                  <tr
                    key={f.id}
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
                    style={{ borderBottom: '1px solid var(--surface-overlay)', background: selectedFiles.has(f.id) ? 'rgba(59, 130, 246, 0.1)' : 'transparent', cursor: 'default' }}
                  >
                    <td style={{ padding: '12px' }}>
                      <input type="checkbox" checked={selectedFiles.has(f.id)} onChange={() => toggleSelection(f.id)} aria-label={`${f.name} seç`} />
                    </td>
                    <td className="file-name-cell" style={{ padding: '12px', display: 'flex', alignItems: 'center', gap: '12px', fontFamily: 'monospace' }}>
                      {getIconForType(f.type)}
                      {f.name}
                    </td>
                    <td style={{ padding: '12px', color: 'var(--text-muted)' }}>{f.size}</td>
                    <td style={{ padding: '12px', color: 'var(--text-muted)', fontSize: '0.85rem', whiteSpace: 'nowrap' }}>{f.dateLabel}</td>
                    <td style={{ padding: '12px' }}>
                      {f.confidenceTier === 'none' ? (
                        <span style={{ color: 'var(--text-muted)', fontSize: '0.8rem' }}>—</span>
                      ) : (
                        <span
                          style={{
                            padding: '3px 8px', borderRadius: '12px', fontSize: '0.78rem', fontWeight: 600,
                            color: tierColor,
                            border: `1px solid ${tierColor}`,
                            background: f.confidenceTier === 'high' ? 'rgba(16, 185, 129, 0.08)' : f.confidenceTier === 'mid' ? 'rgba(245, 158, 11, 0.08)' : 'rgba(239, 68, 68, 0.08)',
                          }}
                          title={f.qualityLabel}
                        >
                          {f.confidence ?? 0}
                        </span>
                      )}
                    </td>
                    <td style={{ padding: '12px', color: 'var(--text-muted)', fontSize: '0.8rem' }}>{f.sourceLabel}</td>
                    <td style={{ padding: '12px' }}>
                      <span style={{
                        padding: '4px 8px', borderRadius: '4px', fontSize: '0.8rem',
                        background: f.status.startsWith('Silinmiş') || f.status.startsWith('Oyulmuş')
                          ? 'rgba(16, 185, 129, 0.1)'
                          : 'rgba(245, 158, 11, 0.1)',
                        color: f.status.startsWith('Silinmiş') || f.status.startsWith('Oyulmuş')
                          ? 'var(--success-green)'
                          : 'var(--warning-yellow)',
                        border: `1px solid ${f.status.startsWith('Silinmiş') || f.status.startsWith('Oyulmuş') ? 'var(--success-green)' : 'var(--warning-yellow)'}`
                      }}>
                        {f.status}
                      </span>
                    </td>
                  </tr>
                  )
                })
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
