import React, { useEffect, useState, useRef, useCallback } from 'react'
import './ScanView.css'
import DiskMapVisualizer from '../DiskMap/DiskMapVisualizer'
import { Search, CheckCircle, ChevronLeft, ChevronRight, File, Square, Pause, AlertTriangle, X } from 'lucide-react'
import { scanProfileLabel } from '../../../shared/scan-profiles'
import { formatSize } from '../ResultsView/results-view-utils'
import {
  etaFromMonotonicWindow,
  formatEtaClock,
  scanStepIndex,
  scanPhaseI18nKey,
  type EtaSample,
} from '../../../shared/scan-eta'
import type { ScanPhase } from '../../../shared/scan-required'
import { useI18n, tFormat, formatInt, localeTag } from '../../i18n'
import InlineAlert from '../InlineAlert'
import { loadScanHonestyFlags } from '../../../shared/scan-honesty'

interface ScanViewProps {
  driveIndex: number | null
  scanType: string
  progress: { current: number; total: number; badSectors?: number[]; phase?: string; phaseCurrent?: number; phaseTotal?: number }
  status: { key: string; err?: string }
  phase: ScanPhase
  elapsed: number
  activeScanId: number
  onStop: () => void
  onCancel: () => void
  onViewResults: () => void
}

const TYPE_CHIPS: { id: string; labelKey: string; category: string }[] = [
  { id: 'all', labelKey: 'scan.all', category: '' },
  { id: 'img', labelKey: 'scan.image', category: 'Image' },
  { id: 'video', labelKey: 'scan.video', category: 'Video' },
  { id: 'audio', labelKey: 'scan.audio', category: 'Audio' },
  { id: 'doc', labelKey: 'scan.document', category: 'Document' },
  { id: 'archive', labelKey: 'scan.archive', category: 'Archive' },
]

function ScanView({
  driveIndex, scanType,
  progress, status, phase,
  elapsed, activeScanId,
  onStop, onCancel, onViewResults
}: ScanViewProps): React.ReactElement {

  const { t } = useI18n()
  const pageRef = useRef(0)
  const [page, setPage] = useState(0)
  const [typeChip, setTypeChip] = useState('all')
  const typeChipRef = useRef(typeChip)
  const [totalFiles, setTotalFiles] = useState(0)
  const [deletedCount, setDeletedCount] = useState(0)
  const [listCount, setListCount] = useState(0)
  const [carvedCount, setCarvedCount] = useState(0)
  const [carveSignatureCount, setCarveSignatureCount] = useState<number | null>(null)
  const [filesFound, setFilesFound] = useState<any[]>([])
  const [listLoading, setListLoading] = useState(false)
  const [selectedFile, setSelectedFile] = useState<any>(null)
  const [hfsTruncated, setHfsTruncated] = useState(false)
  const [hfsCatalogUnread, setHfsCatalogUnread] = useState(false)
  const [apfsNxsbUnread, setApfsNxsbUnread] = useState(false)
  const [refsProbeCapped, setRefsProbeCapped] = useState(false)
  const [refsSupbUnread, setRefsSupbUnread] = useState(false)
  const [fatDirUnread, setFatDirUnread] = useState(false)
  const [ext4DirUnread, setExt4DirUnread] = useState(false)
  const [xfsDirUnread, setXfsDirUnread] = useState(false)
  const [ntfsI30Unread, setNtfsI30Unread] = useState(false)
  const [unallocMapUnread, setUnallocMapUnread] = useState(false)
  const [ntfsLogfileUnread, setNtfsLogfileUnread] = useState(false)
  const [usnUnread, setUsnUnread] = useState(false)
  const [ntfsMftUnread, setNtfsMftUnread] = useState(false)
  const [probeUnread, setProbeUnread] = useState(false)
  const [carverUnread, setCarverUnread] = useState(false)
  const [honestyLoadFailed, setHonestyLoadFailed] = useState(false)
  const [listError, setListError] = useState<string | null>(null)
  const limit = 50

  const speedHistoryRef = useRef<EtaSample[]>([])
  const emaRef = useRef(0)
  const lastPhaseRef = useRef(progress.phase)
  const loadGenRef = useRef(0)
  const [currentSpeed, setCurrentSpeed] = useState(0)
  const [etaSeconds, setEtaSeconds] = useState(-1)
  const [etaStalled, setEtaStalled] = useState(false)

  useEffect(() => { pageRef.current = page }, [page])
  useEffect(() => { typeChipRef.current = typeChip }, [typeChip])

  useEffect(() => {
    if (scanType !== 'deep' && scanType !== 'full_carve' && scanType !== 'carve_only') {
      setCarveSignatureCount(null)
      return
    }
    if (!window.api?.getCarveSignatureCount) return
    void window.api.getCarveSignatureCount()
      .then((n) => setCarveSignatureCount(n))
      .catch(() => setCarveSignatureCount(null))
  }, [scanType])

  useEffect(() => {
    if (lastPhaseRef.current !== progress.phase) {
      lastPhaseRef.current = progress.phase
      speedHistoryRef.current = []
      emaRef.current = 0
    }
    if (phase === 'complete' || phase === 'stopped' || phase === 'paused' || phase === 'failed') {
      setEtaSeconds(-1)
      setCurrentSpeed(0)
      setEtaStalled(false)
      emaRef.current = 0
      speedHistoryRef.current = []
      return
    }
    const r = etaFromMonotonicWindow(
      speedHistoryRef.current,
      progress.current,
      progress.total,
      Date.now(),
      30_000,
      emaRef.current,
    )
    speedHistoryRef.current = r.history
    emaRef.current = r.speed
    setCurrentSpeed(r.speed)
    setEtaSeconds(r.etaSeconds)
    setEtaStalled(r.stalled)
  }, [progress.current, progress.total, progress.phase, phase, elapsed])

  const listFilter = useCallback(() => {
    const chip = TYPE_CHIPS.find((c) => c.id === typeChipRef.current)
    return {
      status: 0,
      category: chip?.category ?? '',
      includeDuplicates: false,
      includeDiscovery: false,
      // Heading says "Silinmiş" — match the Results page, which excludes carved rows.
      sourceNotLike: 'carver%',
    }
  }, [])

  const loadLivePage = useCallback(async () => {
    if (activeScanId <= 0 || !window.api?.getFileCount || !window.api?.getFilesPage) return
    const gen = ++loadGenRef.current
    setListLoading(true)
    const filter = listFilter()
    try {
      const [count, listed, pageData, sum] = await Promise.all([
        window.api.getFileCount(activeScanId),
        window.api.getFileCount(activeScanId, filter),
        window.api.getFilesPage(activeScanId, pageRef.current * limit, limit, filter),
        window.api.getScanSummary ? window.api.getScanSummary(activeScanId) : Promise.resolve(null),
      ])
      if (gen !== loadGenRef.current) return
      setListError(null)
      setTotalFiles(typeof count === 'number' && count >= 0 ? count : 0)
      setListCount(typeof listed === 'number' && listed >= 0 ? listed : 0)
      setFilesFound(pageData ?? [])
      if (sum) {
        setDeletedCount(sum.deletedFiles ?? 0)
        setCarvedCount(sum.carvedFiles ?? 0)
      }
    } catch (e) {
      if (gen !== loadGenRef.current) return
      console.error('Pagination error', e)
      setListError(t('scan.listFailed'))
    } finally {
      if (gen === loadGenRef.current) setListLoading(false)
    }
  }, [activeScanId, listFilter, t])

  useEffect(() => {
    if (driveIndex === null || activeScanId <= 0) return
    void loadLivePage()
    // Terminal phases get one final refresh — polling a finished scan burns
    // IPC forever and lets a tick land after the completion event.
    if (phase === 'complete' || phase === 'stopped' || phase === 'paused' || phase === 'failed') return
    const id = setInterval(() => { void loadLivePage() }, 3000)
    return () => clearInterval(id)
  }, [driveIndex, activeScanId, page, typeChip, loadLivePage, phase])

  useEffect(() => {
    const maxPage = listCount <= 0 ? 0 : Math.max(0, Math.ceil(listCount / limit) - 1)
    if (page > maxPage) setPage(maxPage)
  }, [listCount, page, limit])

  useEffect(() => {
    let cancelled = false
    void loadScanHonestyFlags(activeScanId, window.api?.getFilesPage, filesFound)
      .then((flags) => {
        if (cancelled) return
        setHonestyLoadFailed(false)
        setHfsTruncated(flags.hfsLimit)
        setHfsCatalogUnread(flags.hfsCatalogUnread)
        setApfsNxsbUnread(flags.apfsNxsbUnread)
        setRefsProbeCapped(flags.refsProbeCapped)
        setRefsSupbUnread(flags.refsSupbUnread)
        setFatDirUnread(flags.fatDirUnread)
        setExt4DirUnread(flags.ext4DirUnread)
        setXfsDirUnread(flags.xfsDirUnread)
        setNtfsI30Unread(flags.ntfsI30Unread)
        setUnallocMapUnread(flags.unallocMapUnread)
        setNtfsLogfileUnread(flags.ntfsLogfileUnread)
        setUsnUnread(flags.usnUnread)
        setNtfsMftUnread(flags.ntfsMftUnread)
        setProbeUnread(flags.probeUnread)
        setCarverUnread(flags.carverUnread)
      })
      .catch(() => {
        if (cancelled) return
        setHonestyLoadFailed(true)
      })
    return () => { cancelled = true }
  }, [filesFound, activeScanId])

  const formatElapsed = (seconds: number) => {
    if (seconds < 0) return '—'
    const h = Math.floor(seconds / 3600)
    const m = Math.floor((seconds % 3600) / 60)
    const s = Math.floor(seconds % 60)
    return `${String(h).padStart(2, '0')}:${String(m).padStart(2, '0')}:${String(s).padStart(2, '0')}`
  }

  const formatSpeed = (speed: number) => {
    const unit = t('scan.sectorPerSec')
    if (speed <= 0) return '—'
    if (speed > 1000000) return `${(speed / 1000000).toFixed(2)} M ${unit}`
    if (speed > 1000) return `${(speed / 1000).toFixed(2)} K ${unit}`
    return `${Math.floor(speed)} ${unit}`
  }

  const percent = progress.total > 0
    ? Math.min(100, Math.floor((progress.current / progress.total) * 100))
    : 0
  // CA-017: derive view state from the phase, not from status copy.
  const isFinished = phase === 'complete' || phase === 'stopped'
  const isPaused = phase === 'paused'
  const isFailed = phase === 'failed'
  const isTerminal = isFinished || isPaused || isFailed
  const stopping = phase === 'stopping'
  const scanTitle = phase === 'complete'
    ? t('scan.finished')
    : phase === 'stopped'
      ? t('scan.cancelled')
      : isPaused
        ? t('scan.paused')
        : isFailed
          ? t('scan.failed')
          : tFormat('scan.driveScanning', { drive: driveIndex === -1 ? t('scan.raid') : String(driveIndex) })
  // Profile name: i18n key when the profile exists in the dictionary, engine
  // label otherwise (shared/scan-profiles labels are TR-only).
  const profileKey = `profile.${scanType}.label`
  const profileLabel = t(profileKey) === profileKey ? scanProfileLabel(scanType) : t(profileKey)
  const statusLabel = tFormat(status.key, status.err != null ? { err: status.err } : {})
  const step = scanStepIndex(progress.phase, scanType)
  const remainingLabel = isTerminal
    ? '—'
    : etaStalled
      ? t('scan.noProgress')
      : etaSeconds < 0
        ? t('scan.calculating')
        : formatEtaClock(etaSeconds)
  const rangeStart = listCount === 0 ? 0 : page * limit + 1
  const rangeEnd = Math.min((page + 1) * limit, listCount)

  return (
    <div className="scan-view">
      <div className="scan-header glass-panel">
        <div className="scan-info">
          <div className={`scan-icon${isFinished ? ' ok' : isFailed ? ' fail' : isPaused ? ' warn' : ''}`} aria-hidden="true">
            {isFinished ? <CheckCircle size={20} /> : isFailed ? <AlertTriangle size={20} /> : isPaused ? <Pause size={20} /> : <Search size={20} className="spinner" />}
          </div>
          <div>
            <h2>
              {scanTitle}
            </h2>
            <p>
              {profileLabel} • {statusLabel}
            </p>
          </div>
        </div>
        <div className="scan-stats">
          <div className="stat-pill">
            <span className="pill-label">{t('scan.records')}</span>
            <span className="pill-value">{formatInt(totalFiles)}</span>
            <span className="pill-sub">
              {tFormat('scan.deletedOf', { n: formatInt(deletedCount) })}
              {carvedCount > 0 ? ` · ${tFormat('scan.carvedOf', { n: formatInt(carvedCount) })}` : ''}
            </span>
          </div>
          <div className="stat-pill">
            <span className="pill-label">{t('scan.elapsed')}</span>
            <span className="pill-value">{formatElapsed(elapsed)}</span>
          </div>
          <div className={`stat-pill${isTerminal ? ' dim' : ''}`}>
            <span className="pill-label">{t('scan.remaining')}</span>
            <span className={`pill-value${etaSeconds > 0 && !etaStalled ? ' eta-live' : ''}`}>
              {remainingLabel}
            </span>
          </div>
        </div>
      </div>

      {hfsTruncated && (
        <InlineAlert variant="warning" testId="hfs-limit-banner">{t('scan.hfsLimit')}</InlineAlert>
      )}
      {hfsCatalogUnread && (
        <InlineAlert variant="warning" testId="hfs-catalog-unread">{t('scan.hfsCatalogUnread')}</InlineAlert>
      )}
      {apfsNxsbUnread && (
        <InlineAlert variant="warning" testId="apfs-nxsb-unread">{t('scan.apfsNxsbUnread')}</InlineAlert>
      )}
      {refsProbeCapped && (
        <InlineAlert variant="warning" testId="refs-probe-capped">{t('scan.refsProbeCapped')}</InlineAlert>
      )}
      {refsSupbUnread && (
        <InlineAlert variant="warning" testId="refs-supb-unread">{t('scan.refsSupbUnread')}</InlineAlert>
      )}
      {fatDirUnread && (
        <InlineAlert variant="warning" testId="fat-dir-unread">{t('scan.fatDirUnread')}</InlineAlert>
      )}
      {ext4DirUnread && (
        <InlineAlert variant="warning" testId="ext4-dir-unread">{t('scan.ext4DirUnread')}</InlineAlert>
      )}
      {xfsDirUnread && (
        <InlineAlert variant="warning" testId="xfs-dir-unread">{t('scan.xfsDirUnread')}</InlineAlert>
      )}
      {ntfsI30Unread && (
        <InlineAlert variant="warning" testId="ntfs-i30-unread">{t('scan.ntfsI30Unread')}</InlineAlert>
      )}
      {unallocMapUnread && (
        <InlineAlert variant="warning" testId="unalloc-map-unread">{t('scan.unallocMapUnread')}</InlineAlert>
      )}
      {ntfsLogfileUnread && (
        <InlineAlert variant="warning" testId="ntfs-logfile-unread">{t('scan.ntfsLogfileUnread')}</InlineAlert>
      )}
      {usnUnread && (
        <InlineAlert variant="warning" testId="usn-unread">{t('scan.usnUnread')}</InlineAlert>
      )}
      {ntfsMftUnread && (
        <InlineAlert variant="warning" testId="ntfs-mft-unread">{t('scan.ntfsMftUnread')}</InlineAlert>
      )}
      {probeUnread && (
        <InlineAlert variant="warning" testId="probe-unread">{t('scan.probeUnread')}</InlineAlert>
      )}
      {carverUnread && (
        <InlineAlert variant="warning" testId="carver-unread">{t('scan.carverUnread')}</InlineAlert>
      )}
      {honestyLoadFailed && (
        <InlineAlert variant="warning" testId="scan-honesty-load-error">{t('scan.honestyLoadFailed')}</InlineAlert>
      )}
      {listError && (
        <InlineAlert variant="error" testId="scan-list-error">{listError}</InlineAlert>
      )}
      {progress.badSectors && progress.badSectors.length > 0 && (
        <InlineAlert variant="error">
          {tFormat('scan.badSectors', { n: formatInt(progress.badSectors.length) })}
        </InlineAlert>
      )}
      <div className="glass-panel scan-note" role="note">
        {progress.phase === 'carve_skipped'
          ? t('scan.carveSkipped')
          : scanType === 'carve_only'
          ? tFormat('scan.noteCarveOnly', { step: String(step.step), of: String(step.of), phase: t(scanPhaseI18nKey(progress.phase)), sigs: carveSignatureCount != null ? tFormat('scan.sigs', { n: formatInt(carveSignatureCount) }) : '' })
          : scanType === 'deep' || scanType === 'full_carve'
          ? tFormat('scan.noteCarveDeep', { step: String(step.step), of: String(step.of), phase: t(scanPhaseI18nKey(progress.phase)), sigs: carveSignatureCount != null ? tFormat('scan.sigs', { n: formatInt(carveSignatureCount) }) : '' })
          : t('scan.noteQuick')}
      </div>

      <div className="scan-progress-card glass-panel">
        <DiskMapVisualizer
          totalSectors={progress.total}
          currentSector={progress.current}
          phase={progress.phase}
          filesFound={totalFiles}
          deletedCount={deletedCount}
        />
        <div className="scan-progress-meta">
          <span>{tFormat('scan.stepOf', { step: String(step.step), of: String(step.of) })} · {t(scanPhaseI18nKey(progress.phase))}</span>
          <span className="speed">{formatSpeed(currentSpeed)}</span>
          <span>{tFormat('common.percent', { n: String(percent) })}</span>
        </div>
        <div className="scan-progress-track">
          <div className="scan-progress-fill" style={{ width: `${percent}%` }}></div>
        </div>
        <div className="scan-progress-footer">
          <span>{tFormat('scan.sectorRange', { cur: formatInt(progress.current), total: progress.total ? formatInt(progress.total) : '—' })}</span>
          {progress.phaseCurrent != null && progress.phaseTotal != null && progress.phaseTotal > 0 && (
            <span>{tFormat('scan.phaseProgress', { pct: String(Math.min(100, Math.floor(progress.phaseCurrent * 100 / progress.phaseTotal))) })}</span>
          )}
        </div>
      </div>

      <div className="scan-live-results glass-panel">
        <div className="scan-live-toolbar">
          <h3>
            {tFormat('scan.deletedRange', { range: rangeStart > 0 ? `${rangeStart}–${rangeEnd} / ${formatInt(listCount)}` : '0' })}
            {listLoading ? ' …' : ''}
          </h3>
          <div className="filter-chips" role="group" aria-label={t('scan.fileTypeAria')}>
            {TYPE_CHIPS.map((chip) => (
              <button
                key={chip.id}
                type="button"
                className="filter-chip"
                aria-pressed={typeChip === chip.id}
                onClick={() => {
                  setTypeChip(chip.id)
                  setPage(0)
                }}
              >
                {t(chip.labelKey)}
              </button>
            ))}
          </div>
          <div className="scan-pager">
            <button type="button" className="btn-secondary" disabled={page === 0 || listLoading} onClick={() => setPage(p => p - 1)}>
              <ChevronLeft size={16} aria-hidden="true" /> {t('scan.prev')}
            </button>
            <button type="button" className="btn-secondary" disabled={(page + 1) * limit >= listCount || listLoading} onClick={() => setPage(p => p + 1)}>
              {t('scan.next')} <ChevronRight size={16} aria-hidden="true" />
            </button>
          </div>
        </div>
        <div className="scan-live-body">
          <div className="scan-live-list">
            {filesFound.length === 0 ? (
              <div className="scan-live-empty examiner-empty" role="status" data-testid="scan-live-empty">
                {listError
                  ? t('scan.listFailed')
                  : listLoading
                  ? t('scan.listLoading')
                  : typeChip !== 'all'
                    ? t('scan.noDeletedOfType')
                      : totalFiles > 0
                      ? tFormat('scan.recordSummary', { total: formatInt(totalFiles), deleted: formatInt(deletedCount) })
                      : t('scan.noFiles')}
              </div>
            ) : (
              <div className="scan-file-rows">
                {filesFound.map((f) => {
                  const rowKey = f.id ?? `${f.name}-${f.startSector}`
                  const isSelected = selectedFile && selectedFile.id === f.id
                  return (
                    <button
                      type="button"
                      key={rowKey}
                      onClick={() => setSelectedFile(f)}
                      aria-pressed={!!isSelected}
                      className="scan-file-row"
                    >
                      <File size={18} className="file-ico" aria-hidden="true" />
                      <span className="file-name">{f.name}</span>
                      <span className="file-cat">{f.category}</span>
                      <span className="file-size">
                        {formatSize(f.sizeBytes || f.size || 0)}
                      </span>
                    </button>
                  )
                })}
              </div>
            )}
          </div>

          {selectedFile && (
            <aside className="scan-file-detail">
              <div className="scan-file-detail-head">
                <h4>{t('scan.fileDetail')}</h4>
                <button type="button" className="btn-secondary" onClick={() => setSelectedFile(null)} aria-label={t('common.close')}><X size={14} aria-hidden="true" /></button>
              </div>
              <div className="scan-file-detail-name">
                {selectedFile.name}
              </div>
              {[
                [t('scan.category'), selectedFile.category ?? '—'],
                [t('scan.size'), formatSize(selectedFile.sizeBytes ?? 0)],
                [t('scan.startSector'), selectedFile.startSector?.toLocaleString(localeTag()) ?? '—'],
                [t('scan.endSector'), selectedFile.endSector?.toLocaleString(localeTag()) ?? '—'],
                [t('scan.confidence'), selectedFile.confidence != null ? `${selectedFile.confidence}%` : '—'],
                [t('scan.statusLabel'), selectedFile.status === 0 ? t('scan.deleted') : selectedFile.status === 1 ? t('scan.active') : t('scan.unknown')],
                [t('scan.source'), selectedFile.source ?? '—'],
                [t('scan.runCount'), selectedFile.runs?.length ?? 0],
                [t('scan.created'), selectedFile.createdAt ? new Date(selectedFile.createdAt * 1000).toLocaleString(localeTag()) : '—'],
                [t('scan.modified'), selectedFile.modifiedAt ? new Date(selectedFile.modifiedAt * 1000).toLocaleString(localeTag()) : '—'],
                [t('scan.path'), selectedFile.path ?? '—'],
              ].map(([k, v]) => (
                <div key={String(k)} className="scan-kv">
                  <span className="scan-kv-k">{k}</span>
                  <span className="scan-kv-v">{String(v)}</span>
                </div>
              ))}
            </aside>
          )}
        </div>
      </div>

      <div className="scan-actions">
        {!isTerminal && !stopping && (
          <button type="button" className="btn-danger" onClick={onStop}>
            <Square size={16} fill="currentColor" /> {t('scan.stop')}
          </button>
        )}
        {(isFinished || isPaused) && (
          <button type="button" className="btn-primary" onClick={onViewResults}>
            {t('scan.viewResults')}
          </button>
        )}
        {isTerminal && (
          <button type="button" className="btn-secondary" onClick={onCancel}>{t('scan.backHome')}</button>
        )}
        {stopping && (
          <span className="stopping">{t('scan.stopping')}</span>
        )}
      </div>
    </div>
  )
}

export default ScanView
