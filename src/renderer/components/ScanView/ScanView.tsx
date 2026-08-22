import React, { useEffect, useState, useRef, useCallback } from 'react'
import './ScanView.css'
import DiskMapVisualizer from '../DiskMap/DiskMapVisualizer'
import { Search, CheckCircle, ChevronLeft, ChevronRight, File, Square } from 'lucide-react'
import { scanProfileLabel } from '../../../shared/scan-profiles'
import {
  etaFromMonotonicWindow,
  formatEtaClock,
  scanPhaseLabel,
  scanStepIndex,
  type EtaSample,
} from '../../../shared/scan-eta'

interface ScanViewProps {
  driveIndex: number | null
  scanType: string
  progress: { current: number; total: number; phase?: string }
  status: string
  elapsed: number
  activeScanId: number
  onStop: () => void
  onCancel: () => void
  onViewResults: () => void
}

const TYPE_CHIPS: { id: string; label: string; category: string }[] = [
  { id: 'all', label: 'Tümü', category: '' },
  { id: 'img', label: 'Resim', category: 'Image' },
  { id: 'video', label: 'Video', category: 'Video' },
  { id: 'audio', label: 'Ses', category: 'Audio' },
  { id: 'doc', label: 'Belge', category: 'Document' },
  { id: 'archive', label: 'Arşiv', category: 'Archive' },
]

function ScanView({
  driveIndex, scanType,
  progress, status,
  elapsed, activeScanId,
  onStop, onCancel, onViewResults
}: ScanViewProps): React.ReactElement {

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
    if (status === 'Tarama Tamamlandı' || status === 'Tarama İptal Edildi') {
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
  }, [progress.current, progress.total, progress.phase, status, elapsed])

  const listFilter = useCallback(() => {
    const chip = TYPE_CHIPS.find((c) => c.id === typeChipRef.current)
    return {
      status: 0,
      category: chip?.category ?? '',
      includeDuplicates: false,
      includeDiscovery: false,
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
    } finally {
      if (gen === loadGenRef.current) setListLoading(false)
    }
  }, [activeScanId, listFilter])

  useEffect(() => {
    if (driveIndex === null || activeScanId <= 0) return
    void loadLivePage()
    const id = setInterval(() => { void loadLivePage() }, 3000)
    return () => clearInterval(id)
  }, [driveIndex, activeScanId, page, typeChip, loadLivePage])

  useEffect(() => {
    const maxPage = listCount <= 0 ? 0 : Math.max(0, Math.ceil(listCount / limit) - 1)
    if (page > maxPage) setPage(maxPage)
  }, [listCount, page, limit])

  useEffect(() => {
    if (filesFound.some((f) => f.source === 'hfs_limit')) {
      setHfsTruncated(true)
      return
    }
    if (activeScanId <= 0 || !window.api?.searchFiles) {
      setHfsTruncated(false)
      return
    }
    void window.api
      .searchFiles(activeScanId, 'catalog truncated', 0, 8)
      .then((res) => setHfsTruncated(res.rows.some((r: { source?: string }) => r.source === 'hfs_limit')))
      .catch(() => setHfsTruncated(false))
  }, [filesFound, activeScanId])

  const formatElapsed = (seconds: number) => {
    if (seconds < 0) return '—'
    const h = Math.floor(seconds / 3600)
    const m = Math.floor((seconds % 3600) / 60)
    const s = Math.floor(seconds % 60)
    return `${String(h).padStart(2, '0')}:${String(m).padStart(2, '0')}:${String(s).padStart(2, '0')}`
  }

  const formatSpeed = (speed: number) => {
    if (speed <= 0) return '—'
    if (speed > 1000000) return `${(speed / 1000000).toFixed(2)} M sektör/s`
    if (speed > 1000) return `${(speed / 1000).toFixed(2)} K sektör/s`
    return `${Math.floor(speed)} sektör/s`
  }

  const percent = progress.total > 0
    ? Math.min(100, Math.floor((progress.current / progress.total) * 100))
    : 0
  const isFinished = status === 'Tarama Tamamlandı' || status === 'Tarama İptal Edildi'
  const isPaused = status.includes('Duraklatıldı')
  const isFailed = status.includes('Başarısız') || status.includes('kullanılamıyor')
  const isTerminal = isFinished || isPaused || isFailed
  const stopping = status === 'Durduruluyor...'
  const scanTitle = isFinished
    ? 'Tarama tamamlandı'
    : isPaused
      ? 'Tarama duraklatıldı'
      : isFailed
        ? 'Tarama başarısız'
        : `Sürücü ${driveIndex === -1 ? 'RAID' : driveIndex} taranıyor`
  const step = scanStepIndex(progress.phase, scanType)
  const remainingLabel = isFinished
    ? formatElapsed(0)
    : etaStalled
      ? 'ilerleme yok'
      : etaSeconds < 0
        ? 'hesaplanıyor'
        : formatEtaClock(etaSeconds)
  const rangeStart = listCount === 0 ? 0 : page * limit + 1
  const rangeEnd = Math.min((page + 1) * limit, listCount)

  return (
    <div className="scan-view" style={{ display: 'flex', flexDirection: 'column', gap: 'var(--space-lg)' }}>
      
      <div className="scan-header glass-panel" style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center', padding: 'var(--space-xl)' }}>
        <div className="scan-info" style={{ display: 'flex', alignItems: 'center', gap: 'var(--space-md)' }}>
          <div className="scan-icon" style={{ 
            background: isFinished ? 'rgba(16, 185, 129, 0.1)' : 'rgba(59, 130, 246, 0.1)', 
            padding: '16px', borderRadius: '12px',
            color: isFinished ? 'var(--success-green)' : 'var(--accent-blue)'
          }}>
            {isFinished ? <CheckCircle size={32} /> : <Search size={32} className="spinner" />}
          </div>
          <div>
            <h2 style={{ fontSize: '1.5rem', marginBottom: '4px' }}>
              {scanTitle}
            </h2>
            <p style={{ color: 'var(--text-muted)' }}>
              {scanProfileLabel(scanType)} • {status}
            </p>
          </div>
        </div>
        <div className="scan-stats" style={{ display: 'flex', gap: 'var(--space-md)' }}>
          <div className="stat-pill" style={{ background: 'rgba(255,255,255,0.03)', padding: '12px 24px', borderRadius: '8px', textAlign: 'center' }}>
            <span style={{ display: 'block', fontSize: '0.75rem', color: 'var(--text-muted)', textTransform: 'uppercase', letterSpacing: '0.05em' }}>Kayıt</span>
            <span style={{ display: 'block', fontSize: '1.25rem', fontWeight: 600 }}>{totalFiles.toLocaleString('tr-TR')}</span>
            <span style={{ display: 'block', fontSize: '0.7rem', color: 'var(--text-muted)' }}>
              silinmiş {deletedCount.toLocaleString('tr-TR')}
              {carvedCount > 0 ? ` · oyulmuş ${carvedCount.toLocaleString('tr-TR')}` : ''}
            </span>
          </div>
          <div className="stat-pill" style={{ background: 'rgba(255,255,255,0.03)', padding: '12px 24px', borderRadius: '8px', textAlign: 'center' }}>
            <span style={{ display: 'block', fontSize: '0.75rem', color: 'var(--text-muted)', textTransform: 'uppercase', letterSpacing: '0.05em' }}>Geçen Süre</span>
            <span style={{ display: 'block', fontSize: '1.25rem', fontWeight: 600 }}>{formatElapsed(elapsed)}</span>
          </div>
          <div className="stat-pill" style={{ background: 'rgba(255,255,255,0.03)', padding: '12px 24px', borderRadius: '8px', textAlign: 'center', opacity: isFinished ? 0.3 : 1 }}>
            <span style={{ display: 'block', fontSize: '0.75rem', color: 'var(--text-muted)', textTransform: 'uppercase', letterSpacing: '0.05em' }}>Kalan Süre</span>
            <span style={{ display: 'block', fontSize: '1.25rem', fontWeight: 600, color: etaSeconds > 0 && !etaStalled ? 'var(--accent-blue)' : 'inherit' }}>
              {remainingLabel}
            </span>
          </div>
        </div>
      </div>

      {hfsTruncated && (
        <div className="glass-panel" role="alert" style={{ padding: '16px 24px', borderLeft: '4px solid var(--warning-yellow)' }}>
          HFS+ katalog limit sentinel kaydı var. Varsayılan yürüyüş sınırsız; bu uyarı yalnız limit verilmiş taramada çıkar.
        </div>
      )}
      <div className="glass-panel" role="note" style={{ padding: '12px 24px', color: 'var(--text-muted)', fontSize: '0.85rem' }}>
        {progress.phase === 'carve_skipped'
          ? 'Oyma atlandı — bu dosya sistemi için boş alan haritası yok (APFS/HFS/ReFS). Tam disk carve veya carve_only profilini dene.'
          : scanType === 'carve_only'
          ? `Adım ${step.step}/${step.of}: ${scanPhaseLabel(progress.phase)}. ${carveSignatureCount != null ? `${carveSignatureCount.toLocaleString('tr-TR')} imza.` : ''} Dosya sistemi atlandı — yalnız imza carve. Sonuçlara istediğin zaman geç.`
          : scanType === 'deep' || scanType === 'full_carve'
          ? `Adım ${step.step}/${step.of}: ${scanPhaseLabel(progress.phase)}. ${carveSignatureCount != null ? `${carveSignatureCount.toLocaleString('tr-TR')} imza.` : ''} Sonuçlara istediğin zaman geç — tarama arka planda sürer. %75 civarı metadata bitişi; sonrası oyma ve uzun sürebilir.`
          : 'Hızlı tarama yalnız dosya tablosu okur. Boş alandaki foto/video için Derin tarama.'}
      </div>

      <div className="scan-progress-card glass-panel" style={{ padding: 'var(--space-xl)' }}>
        <DiskMapVisualizer
          totalSectors={progress.total}
          currentSector={progress.current}
          phase={progress.phase}
          filesFound={totalFiles}
          deletedCount={deletedCount}
        />
        <div style={{ display: 'flex', justifyContent: 'space-between', marginTop: 'var(--space-md)', fontSize: '0.875rem', color: 'var(--text-muted)' }}>
          <span>Adım {step.step}/{step.of} · {scanPhaseLabel(progress.phase)}</span>
          <span style={{ color: 'var(--accent-blue)', fontFamily: 'monospace' }}>{formatSpeed(currentSpeed)}</span>
          <span>%{percent}</span>
        </div>
        <div style={{ width: '100%', height: '6px', background: 'rgba(255,255,255,0.1)', borderRadius: '3px', marginTop: '8px', overflow: 'hidden' }}>
          <div style={{ width: `${percent}%`, height: '100%', background: 'var(--accent-blue)', transition: 'width 0.3s ease' }}></div>
        </div>
        <div style={{ marginTop: '8px', fontSize: '0.75rem', color: 'var(--text-muted)' }}>
          Sektör {progress.current.toLocaleString('tr-TR')} / {progress.total ? progress.total.toLocaleString('tr-TR') : '—'}
        </div>
      </div>

      <div className="scan-live-results glass-panel" style={{ flex: 1, display: 'flex', flexDirection: 'column', minHeight: '300px' }}>
        <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center', padding: 'var(--space-md) var(--space-xl)', borderBottom: '1px solid var(--panel-border)', flexWrap: 'wrap', gap: '8px' }}>
          <h3 style={{ fontSize: '1rem', fontWeight: 500 }}>
            Silinmiş {rangeStart > 0 ? `${rangeStart}–${rangeEnd} / ${listCount.toLocaleString('tr-TR')}` : '0'}
            {listLoading ? ' …' : ''}
          </h3>
          <div className="filter-chips" role="group" aria-label="Dosya tipi">
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
                {chip.label}
              </button>
            ))}
          </div>
          <div style={{ display: 'flex', gap: 'var(--space-sm)' }}>
            <button className="btn-secondary" style={{ padding: '6px 12px' }} disabled={page === 0 || listLoading} onClick={() => setPage(p => p - 1)}>
              <ChevronLeft size={16} /> Önceki
            </button>
            <button className="btn-secondary" style={{ padding: '6px 12px' }} disabled={(page + 1) * limit >= listCount || listLoading} onClick={() => setPage(p => p + 1)}>
              Sonraki <ChevronRight size={16} />
            </button>
          </div>
        </div>
        <div style={{ padding: 'var(--space-md)', overflowY: 'auto', flex: 1, display: 'flex', gap: 'var(--space-md)' }}>
          <div style={{ flex: 1, minWidth: 0 }}>
            {filesFound.length === 0 ? (
              <div style={{ textAlign: 'center', color: 'var(--text-muted)', marginTop: '2rem' }}>
                {listLoading
                  ? 'Liste yükleniyor…'
                  : typeChip !== 'all'
                    ? 'Bu tipte silinmiş kayıt yok. Uzantısız dosyalar Tümü süzgecinde.'
                    : totalFiles > 0
                      ? `Kayıt: ${totalFiles.toLocaleString('tr-TR')} (silinmiş: ${deletedCount.toLocaleString('tr-TR')})`
                      : 'Henüz dosya bulunamadı...'}
              </div>
            ) : (
              <div style={{ display: 'flex', flexDirection: 'column', gap: '4px' }}>
                {filesFound.map((f) => {
                  const rowKey = f.id ?? `${f.name}-${f.startSector}`
                  const isSelected = selectedFile && selectedFile.id === f.id
                  return (
                    <button
                      type="button"
                      key={rowKey}
                      onClick={() => setSelectedFile(f)}
                      aria-pressed={!!isSelected}
                      style={{
                      display: 'flex', alignItems: 'center', padding: '12px 16px', width: '100%',
                      background: isSelected ? 'rgba(59, 130, 246, 0.08)' : 'rgba(255,255,255,0.02)', borderRadius: '6px', cursor: 'pointer',
                      border: `1px solid ${isSelected ? 'rgba(59, 130, 246, 0.4)' : 'transparent'}`, transition: 'all 0.2s', color: 'inherit', textAlign: 'left'
                    }}
                    onMouseEnter={(e) => { if (!isSelected) e.currentTarget.style.borderColor = 'var(--panel-border)' }}
                    onMouseLeave={(e) => { if (!isSelected) e.currentTarget.style.borderColor = 'transparent' }}
                    >
                      <File size={18} style={{ color: 'var(--accent-blue)', marginRight: '12px' }} />
                      <span style={{ fontWeight: 500, overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }}>{f.name}</span>
                      <span style={{ marginLeft: '16px', fontSize: '0.8rem', color: 'var(--text-muted)', background: 'rgba(255,255,255,0.05)', padding: '2px 8px', borderRadius: '12px', flexShrink: 0 }}>{f.category}</span>
                      <span style={{ marginLeft: 'auto', color: 'var(--text-muted)', fontSize: '0.9rem', flexShrink: 0 }}>
                        {(f.sizeBytes ? f.sizeBytes : f.size) ? ((f.sizeBytes || f.size) / 1024).toFixed(2) : 0} KB
                      </span>
                    </button>
                  )
                })}
              </div>
            )}
          </div>

          {selectedFile && (
            <div style={{ width: '320px', flexShrink: 0, background: 'rgba(0,0,0,0.2)', borderRadius: '8px', padding: 'var(--space-md)', border: '1px solid var(--panel-border)', overflowY: 'auto' }}>
              <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center', marginBottom: 'var(--space-md)' }}>
                <h4 style={{ fontSize: '0.9rem', color: 'var(--text-muted)', textTransform: 'uppercase', letterSpacing: '0.05em' }}>Dosya Detayı</h4>
                <button className="btn-secondary" style={{ padding: '2px 8px', fontSize: '0.8rem' }} onClick={() => setSelectedFile(null)}>✕</button>
              </div>
              <div style={{ fontFamily: 'monospace', fontSize: '0.9rem', wordBreak: 'break-all', marginBottom: 'var(--space-md)', color: 'var(--text-main)' }}>
                {selectedFile.name}
              </div>
              {[
                ['Kategori', selectedFile.category ?? '—'],
                ['Boyut', ((selectedFile.sizeBytes ?? 0) / 1024).toFixed(2) + ' KB'],
                ['Başlangıç Sektörü', selectedFile.startSector?.toLocaleString() ?? '—'],
                ['Bitiş Sektörü', selectedFile.endSector?.toLocaleString() ?? '—'],
                ['Güven Skoru', selectedFile.confidence != null ? `${selectedFile.confidence}%` : '—'],
                ['Durum', selectedFile.status === 0 ? 'Silinmiş' : selectedFile.status === 1 ? 'Aktif' : 'Bilinmiyor'],
                ['Kaynak', selectedFile.source ?? '—'],
                ['Data Run Sayısı', selectedFile.runs?.length ?? 0],
                ['Oluşturma', selectedFile.createdAt ? new Date(selectedFile.createdAt * 1000).toLocaleString('tr-TR') : '—'],
                ['Değiştirme', selectedFile.modifiedAt ? new Date(selectedFile.modifiedAt * 1000).toLocaleString('tr-TR') : '—'],
                ['Yol', selectedFile.path ?? '—'],
              ].map(([k, v]) => (
                <div key={String(k)} style={{ display: 'flex', justifyContent: 'space-between', gap: '12px', padding: '6px 0', borderBottom: '1px solid rgba(255,255,255,0.04)', fontSize: '0.8rem' }}>
                  <span style={{ color: 'var(--text-muted)', flexShrink: 0 }}>{k}</span>
                  <span style={{ textAlign: 'right', wordBreak: 'break-all' }}>{String(v)}</span>
                </div>
              ))}
            </div>
          )}
        </div>
      </div>

      <div className="scan-actions" style={{ display: 'flex', gap: 'var(--space-md)', justifyContent: 'flex-end', marginTop: 'var(--space-md)' }}>
        {!isTerminal && !stopping && (
          <button className="btn-danger" onClick={onStop} style={{ display: 'flex', alignItems: 'center', gap: '8px' }}>
            <Square size={16} fill="currentColor" /> Taramayı Durdur
          </button>
        )}
        {(isFinished || isPaused) && (
          <button className="btn-primary" onClick={onViewResults}>
            Sonuçları Görüntüle
          </button>
        )}
        {isTerminal && (
          <button className="btn-secondary" onClick={onCancel}>Ana ekran</button>
        )}
        {stopping && (
          <span style={{ color: 'var(--text-muted)', alignSelf: 'center' }}>Native tarama duruyor…</span>
        )}
      </div>

    </div>
  )
}

export default ScanView
