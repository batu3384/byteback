import React, { useState, useEffect, useCallback } from 'react'
import './ResultsView.css'
import { File, FileImage, FileText, FileVideo, FileAudio, FileArchive, Download, ShieldCheck, Folder, FolderOpen, ListTree, List, Eye } from 'lucide-react'
import type { FileRecord, FilePreviewResult } from '../../../shared/ipc-contract'
import { sourceDisplayLabel, isDiscoveryOnlySource, canRecoverSource, isRecoverableListSource, isDuplicateSource } from '../../../shared/source-label'
import { csvCell } from '../../../shared/html-escape'
import ResultsPreviewPanel from './ResultsPreviewPanel'
import {
  qualityHint,
  getExtension,
  getFileType,
  formatSize,
  buildTree,
  type MappedFile,
  type TreeNode,
} from './results-view-utils'

interface ResultsViewProps {
  filesFound: any[]
  driveIndex: number | null
  scanId?: number
}

const PAGE_SIZE = 500

function ResultsView({ filesFound, driveIndex, scanId }: ResultsViewProps): React.ReactElement {
  const [filter, setFilter] = useState('all')
  const [showDuplicates, setShowDuplicates] = useState(false)
  const [selectedFiles, setSelectedFiles] = useState<Set<number>>(new Set())
  const [isRecovering, setIsRecovering] = useState(false)
  const [viewMode, setViewMode] = useState<'tree' | 'flat'>('tree')
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

  const effectiveScanId = scanId && scanId > 0 ? scanId : -1

  const loadPreview = async (fileId: number) => {
    if (effectiveScanId <= 0 || !window.api?.readFilePreview) {
      setPreview({ success: false, error: 'Önizleme için tamamlanmış tarama gerekli.' })
      return
    }
    const raidState = window.api?.getRaidState ? await window.api.getRaidState() : { active: false }
    const effectiveDrive = driveIndex !== null ? driveIndex : -1
    if (effectiveDrive < 0 && !raidState.active) {
      setPreview({ success: false, error: 'Önizleme için sürücü veya RAID gerekli.' })
      return
    }
    setPreviewLoading(true)
    setPreviewTargetId(fileId)
    try {
      const res = await window.api.readFilePreview(effectiveDrive, effectiveScanId, fileId)
      setPreview(res)
    } catch {
      setPreview({ success: false, error: 'Önizleme okunamadı.' })
    } finally {
      setPreviewLoading(false)
    }
  }

  const loadPage = useCallback(async (scan: number, pageIndex: number) => {
    if (!window.api?.getFilesPage || !window.api?.getFileCount || scan <= 0) return
    setLoading(true)
    try {
      const [count, pageData] = await Promise.all([
        window.api.getFileCount(scan),
        window.api.getFilesPage(scan, pageIndex * PAGE_SIZE, PAGE_SIZE),
      ])
      setTotalCount(count)
      setDbFiles(pageData)
      setRecordById(prev => {
        const next = new Map(prev)
        for (const f of pageData) next.set(f.id, f)
        return next
      })
    } finally {
      setLoading(false)
    }
  }, [])

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

  const sourceFiles: FileRecord[] = (effectiveScanId > 0
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
      }))).filter((f) => isRecoverableListSource(f.source) || (showDuplicates && isDuplicateSource(f.source)))

  const handleRecover = async () => {
    if (effectiveScanId <= 0) {
      setRecoverReport('Kurtarma yalnız tarama veritabanındaki kayıtlardan yapılır. Tarama bitsin, sonra sonuç listesinden seçin.')
      return
    }
    if (selectedFiles.size === 0) return
    const raidState = window.api?.getRaidState ? await window.api.getRaidState() : { active: false }
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
    let destDir = await window.api.pickDirectory()
    if (!destDir) {
      setIsRecovering(false)
      return
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
      setIsRecovering(false)
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

    const noteValidation = (res: { validationScore?: number }) => {
      if (typeof res.validationScore !== 'number' || res.validationScore < 0) return
      if (res.validationScore >= 60) validatedOk++
      else validatedBad++
    }

    const noteResult = (res: { success?: boolean; zeroFilled?: boolean; error?: string; validationScore?: number }, id: number) => {
      if (res.success) successCount++
      else {
        failedCount++
        if (res.error) errors.push(`#${id}: ${res.error}`)
      }
      if (res.zeroFilled) zeroFilledCount++
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
        successCount = res.succeeded
        failedCount = res.failed
        zeroFilledCount = (res.results ?? []).filter((r) => r.zeroFilled).length
        for (const r of res.results ?? []) {
          if (!r.success && r.error) errors.push(r.error)
          noteValidation(r)
        }
      } catch {
        failedCount = fileIds.length
        errors.push('Toplu kurtarma istisnası')
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
          noteResult(res, fileId)
        } catch {
          failedCount++
          errors.push(`#${fileId}: istisna`)
        }
      }
    }

    setIsRecovering(false)
    const skipLine = skipped.length ? `\nAtlanan keşif kaydı: ${skipped.length}` : ''
    const validationLine =
      validatedOk + validatedBad > 0
        ? `\nDoğrulama (carve): Tam ${validatedOk}, Bozuk ${validatedBad}`
        : ''
    const errLine = errors.length ? `\nHatalar:\n${errors.slice(0, 8).join('\n')}` : ''
    setRecoverReport(
      `Kurtarma bitti. Başarılı: ${successCount}. Başarısız: ${failedCount}. Eksik/pad okuma: ${zeroFilledCount}. Hedef: ${destDir}${skipLine}${validationLine}${errLine}`,
    )
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

  const exportCsv = () => {
    const rows = filteredFiles.map((f) => {
      const raw: FileRecord | undefined = recordById.get(f.id) ?? sourceFiles.find((x) => x.id === f.id)
      return {
        name: f.name,
        sizeBytes: raw?.sizeBytes ?? '',
        category: raw?.category ?? '',
        confidence: raw?.confidence ?? '',
        status: raw?.status ?? '',
        path: raw?.path ?? '',
        source: raw?.source ?? f.sourceLabel ?? '',
        startSector: raw?.startSector ?? '',
        createdAt: raw?.createdAt ? new Date(raw.createdAt * 1000).toISOString() : '',
        modifiedAt: raw?.modifiedAt ? new Date(raw.modifiedAt * 1000).toISOString() : '',
      }
    })
    const header = ['name', 'sizeBytes', 'category', 'confidence', 'status', 'path', 'source', 'startSector', 'createdAt', 'modifiedAt']
    const csv = [header.join(';'), ...rows.map((r) => header.map((h) => csvCell((r as any)[h])).join(';'))].join('\r\n')
    const blob = new Blob(['\uFEFF' + csv], { type: 'text/csv;charset=utf-8' })
    const url = URL.createObjectURL(blob)
    const a = document.createElement('a')
    a.href = url
    a.download = `byteback-sonuclar-${new Date().toISOString().slice(0, 10)}.csv`
    a.click()
    URL.revokeObjectURL(url)
  }

  const toggleAll = (e: React.ChangeEvent<HTMLInputElement>) => {
    if (e.target.checked) {
      setSelectedFiles(new Set(filteredFiles.map(f => f.id)))
    } else {
      setSelectedFiles(new Set())
    }
  }

  const mappedFiles: MappedFile[] = sourceFiles.map((f) => ({
    id: f.id,
    name: f.name,
    rawPath: typeof f.path === 'string' ? f.path : '',
    rawStatus: f.status ?? 0,
    size: formatSize(f.sizeBytes || 0),
    path: typeof f.path === 'string' && f.path ? f.path : '—',
    status: f.status === 1 ? 'Allocated / in-use' : 'Silinmiş / unallocated',
    type: getFileType(getExtension(f.name)),
    sourceLabel: sourceDisplayLabel(f.source),
  }))

  const filteredFiles = mappedFiles.filter(f => {
    if (filter === 'deleted') return f.rawStatus === 0
    return filter === 'all' || f.type === filter
  })

  const treeRoot = buildTree(filteredFiles)

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
            Taranan disk üzerinde tespit edilen {displayTotal.toLocaleString('tr-TR')} dosya
            {effectiveScanId > 0 && totalPages > 1 ? ` — sayfa ${page + 1}/${totalPages}` : ''}
            {loading ? ' (yükleniyor...)' : ''}
          </p>
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
          <button className="btn-secondary" style={{ display: 'flex', gap: '8px' }} onClick={exportCsv} disabled={filteredFiles.length === 0}>
            <Download size={16} /> Dışa Aktar (CSV)
          </button>
          <button 
            className="btn-primary" 
            style={{ display: 'flex', gap: '8px', opacity: selectedFiles.size === 0 ? 0.5 : 1, cursor: selectedFiles.size === 0 ? 'not-allowed' : 'pointer' }}
            onClick={handleRecover}
            disabled={selectedFiles.size === 0 || isRecovering}
          >
            <ShieldCheck size={16} /> 
            {isRecovering ? 'Kurtarılıyor...' : `Seçilenleri Kurtar (${selectedFiles.size})`}
          </button>
        </div>
      </div>

      {recoverReport && (
        <div className="glass-panel" role="status" style={{ padding: '16px 24px', borderLeft: '4px solid var(--accent-blue)', whiteSpace: 'pre-wrap' }}>
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

      {effectiveScanId > 0 && totalPages > 1 && (
        <div style={{ display: 'flex', gap: '8px', justifyContent: 'center' }}>
          <button className="btn-secondary" disabled={page === 0 || loading} onClick={() => setPage(p => Math.max(0, p - 1))}>Önceki</button>
          <button className="btn-secondary" disabled={page >= totalPages - 1 || loading} onClick={() => setPage(p => p + 1)}>Sonraki</button>
          <span style={{ color: 'var(--text-muted)', fontSize: '0.85rem' }}>Seçim bu sayfadaki kayıtlara uygulanır ({PAGE_SIZE}/sayfa).</span>
        </div>
      )}

      <div className="results-content glass-panel" style={{ flex: 1, display: 'flex', flexDirection: 'column', overflow: 'hidden' }}>
        <div className="filters" style={{ display: 'flex', gap: '8px', padding: '16px 24px', borderBottom: '1px solid var(--panel-border)', alignItems: 'center', flexWrap: 'wrap' }}>
          <button
            className="btn-secondary"
            title={viewMode === 'tree' ? 'Düz liste görünümüne geç' : 'Dizin ağacı görünümüne geç'}
            onClick={() => setViewMode(viewMode === 'tree' ? 'flat' : 'tree')}
            style={{ padding: '6px 12px', display: 'flex', gap: '6px', alignItems: 'center' }}
          >
            {viewMode === 'tree' ? <List size={16} /> : <ListTree size={16} />}
            {viewMode === 'tree' ? 'Liste' : 'Ağaç'}
          </button>
          <button className={`btn-secondary ${filter === 'all' ? 'active' : ''}`} style={{ padding: '6px 16px', background: filter === 'all' ? 'var(--panel-border)' : 'transparent' }} onClick={() => setFilter('all')}>Tümü</button>
          <button className={`btn-secondary ${filter === 'deleted' ? 'active' : ''}`} style={{ padding: '6px 16px', background: filter === 'deleted' ? 'var(--panel-border)' : 'transparent' }} onClick={() => setFilter('deleted')} data-testid="filter-deleted">Yalnız silinmiş</button>
          <label style={{ display: 'flex', alignItems: 'center', gap: '6px', marginLeft: '8px', fontSize: '13px' }}>
            <input type="checkbox" checked={showDuplicates} onChange={(e) => setShowDuplicates(e.target.checked)} data-testid="show-duplicates" />
            Tekrarları göster
          </label>
          <button className={`btn-secondary ${filter === 'img' ? 'active' : ''}`} style={{ padding: '6px 16px', background: filter === 'img' ? 'var(--panel-border)' : 'transparent' }} onClick={() => setFilter('img')}>Resimler</button>
          <button className={`btn-secondary ${filter === 'doc' ? 'active' : ''}`} style={{ padding: '6px 16px', background: filter === 'doc' ? 'var(--panel-border)' : 'transparent' }} onClick={() => setFilter('doc')}>Belgeler</button>
          <button className={`btn-secondary ${filter === 'video' ? 'active' : ''}`} style={{ padding: '6px 16px', background: filter === 'video' ? 'var(--panel-border)' : 'transparent' }} onClick={() => setFilter('video')}>Videolar</button>
          <button className={`btn-secondary ${filter === 'audio' ? 'active' : ''}`} style={{ padding: '6px 16px', background: filter === 'audio' ? 'var(--panel-border)' : 'transparent' }} onClick={() => setFilter('audio')}>Ses</button>
          <button className={`btn-secondary ${filter === 'archive' ? 'active' : ''}`} style={{ padding: '6px 16px', background: filter === 'archive' ? 'var(--panel-border)' : 'transparent' }} onClick={() => setFilter('archive')}>Arşivler</button>
        </div>

        <div style={{ flex: 1, overflowY: 'auto', padding: '0 24px' }} className={viewMode === 'tree' ? 'tree-container' : ''}>
          {viewMode === 'tree' ? (
          <div style={{ padding: '12px 0', display: 'flex', flexDirection: 'column', gap: '2px' }}>
            {filteredFiles.length === 0 ? (
              <div style={{ textAlign: 'center', padding: '3rem', color: 'var(--text-muted)' }}>Bu kategoride dosya bulunamadı.</div>
            ) : renderTreeNode(treeRoot, 0)}
          </div>
          ) : (
          <table className="results-table" style={{ width: '100%', borderCollapse: 'collapse', textAlign: 'left' }}>
            <thead style={{ position: 'sticky', top: 0, background: 'var(--bg-surface)', zIndex: 1 }}>
              <tr>
                <th style={{ padding: '12px', borderBottom: '1px solid var(--panel-border)', width: '40px' }}>
                  <input type="checkbox" checked={selectedFiles.size === filteredFiles.length && filteredFiles.length > 0} onChange={toggleAll} />
                </th>
                <th style={{ padding: '12px', borderBottom: '1px solid var(--panel-border)' }}>Dosya Adı</th>
                <th style={{ padding: '12px', borderBottom: '1px solid var(--panel-border)' }}>Boyut</th>
                <th style={{ padding: '12px', borderBottom: '1px solid var(--panel-border)' }}>Konum</th>
                <th style={{ padding: '12px', borderBottom: '1px solid var(--panel-border)' }}>Kaynak</th>
                <th style={{ padding: '12px', borderBottom: '1px solid var(--panel-border)' }}>Kalite</th>
                <th style={{ padding: '12px', borderBottom: '1px solid var(--panel-border)' }}>Durum</th>
              </tr>
            </thead>
            <tbody>
              {filteredFiles.length === 0 ? (
                <tr>
                  <td colSpan={7} style={{ textAlign: 'center', padding: '3rem', color: 'var(--text-muted)' }}>
                    Bu kategoride dosya bulunamadı.
                  </td>
                </tr>
              ) : (
                filteredFiles.map((f) => {
                  const raw = recordById.get(f.id) ?? sourceFiles.find((x) => x.id === f.id)
                  const kalite = qualityHint(raw)
                  return (
                  <tr key={f.id} style={{ borderBottom: '1px solid rgba(255,255,255,0.02)', background: selectedFiles.has(f.id) ? 'rgba(59, 130, 246, 0.1)' : 'transparent' }}>
                    <td style={{ padding: '12px' }}>
                      <input type="checkbox" checked={selectedFiles.has(f.id)} onChange={() => toggleSelection(f.id)} />
                    </td>
                    <td className="file-name-cell" style={{ padding: '12px', display: 'flex', alignItems: 'center', gap: '12px', fontFamily: 'monospace' }}>
                      {getIconForType(f.type)}
                      {f.name}
                    </td>
                    <td style={{ padding: '12px', color: 'var(--text-muted)' }}>{f.size}</td>
                    <td style={{ padding: '12px', color: 'var(--text-muted)' }}>{f.path}</td>
                    <td style={{ padding: '12px', color: 'var(--text-muted)', fontSize: '0.8rem' }}>{f.sourceLabel}</td>
                    <td style={{ padding: '12px', fontSize: '0.8rem', color: kalite === 'Zayıf' ? 'var(--alert-red)' : kalite === 'Şüpheli' ? 'var(--warning-yellow)' : 'var(--text-muted)' }}>
                      {kalite}
                    </td>
                    <td style={{ padding: '12px' }}>
                      <span style={{ 
                        padding: '4px 8px', borderRadius: '4px', fontSize: '0.8rem',
                        background: f.status.startsWith('Silinmiş') ? 'rgba(16, 185, 129, 0.1)' : 'rgba(245, 158, 11, 0.1)',
                        color: f.status.startsWith('Silinmiş') ? 'var(--success-green)' : 'var(--warning-yellow)',
                        border: `1px solid ${f.status.startsWith('Silinmiş') ? 'var(--success-green)' : 'var(--warning-yellow)'}`
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
