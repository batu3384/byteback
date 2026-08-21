import React, { useEffect, useState, useRef } from 'react'
import './ScanView.css'
import DiskMapVisualizer from '../DiskMap/DiskMapVisualizer'
import { Search, CheckCircle, ChevronLeft, ChevronRight, File, Square } from 'lucide-react'
import { scanProfileLabel } from '../../../shared/scan-profiles'
import { etaFromMonotonicWindow, type EtaSample } from '../../../shared/scan-eta'

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

function ScanView({
  driveIndex, scanType,
  progress, status,
  elapsed, activeScanId,
  onStop, onCancel, onViewResults
}: ScanViewProps): React.ReactElement {

  const pollRef = useRef<ReturnType<typeof setInterval> | null>(null)
  const pageRef = useRef(0)
  const [page, setPage] = useState(0)
  const [totalFiles, setTotalFiles] = useState(0)
  const [deletedCount, setDeletedCount] = useState(0)
  const [listCount, setListCount] = useState(0)
  const [carvedCount, setCarvedCount] = useState(0)
  const [carveSignatureCount, setCarveSignatureCount] = useState<number | null>(null)
  const [filesFound, setFilesFound] = useState<any[]>([])
  const [selectedFile, setSelectedFile] = useState<any>(null)
  const [hfsTruncated, setHfsTruncated] = useState(false)
  const limit = 50

  const speedHistoryRef = useRef<EtaSample[]>([])
  const emaRef = useRef(0)
  const [currentSpeed, setCurrentSpeed] = useState(0)
  const [etaSeconds, setEtaSeconds] = useState(-1)

  useEffect(() => { pageRef.current = page }, [page])

  useEffect(() => {
    if (scanType !== 'deep' && scanType !== 'full_carve') {
      setCarveSignatureCount(null)
      return
    }
    if (!window.api?.getCarveSignatureCount) return
    void window.api.getCarveSignatureCount()
      .then((n) => setCarveSignatureCount(n))
      .catch(() => setCarveSignatureCount(null))
  }, [scanType])

  useEffect(() => {
    if (status === 'Tarama Tamamlandı' || status === 'Tarama İptal Edildi') {
      setEtaSeconds(-1)
      setCurrentSpeed(0)
      emaRef.current = 0
      speedHistoryRef.current = []
      return
    }
    const r = etaFromMonotonicWindow(
      speedHistoryRef.current,
      progress.current,
      progress.total,
      Date.now(),
      5000,
      emaRef.current,
    )
    speedHistoryRef.current = r.history
    emaRef.current = r.speed
    setCurrentSpeed(r.speed)
    setEtaSeconds(r.etaSeconds)
  }, [progress.current, progress.total, status])

  useEffect(() => {
    if (driveIndex === null || activeScanId <= 0) return

    pollRef.current = setInterval(async () => {
      if (!window.api?.getFileCount || !window.api?.getFilesPage) return
      try {
        const deletedFilter = { status: 0, includeDuplicates: false, includeDiscovery: false }
        const count = await window.api.getFileCount(activeScanId)
        setTotalFiles(count)
        const deletedListed = await window.api.getFileCount(activeScanId, deletedFilter)
        setListCount(deletedListed)
        if (window.api.getScanSummary) {
          const sum = await window.api.getScanSummary(activeScanId)
          setDeletedCount(sum.deletedFiles ?? 0)
          setCarvedCount(sum.carvedFiles ?? 0)
        }
        const pageData = await window.api.getFilesPage(activeScanId, pageRef.current * limit, limit, deletedFilter)
        if (pageData) setFilesFound(pageData)
      } catch (e) {
        console.error('Pagination error', e)
      }
    }, 1500)

    return () => {
      if (pollRef.current) clearInterval(pollRef.current)
    }
  }, [driveIndex, activeScanId, page])

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

  const formatTime = (seconds: number) => {
    if (seconds < 0) return "--:--:--"
    const h = Math.floor(seconds / 3600)
    const m = Math.floor((seconds % 3600) / 60)
    const s = Math.floor(seconds % 60)
    return `${String(h).padStart(2, '0')}:${String(m).padStart(2, '0')}:${String(s).padStart(2, '0')}`
  }

  const formatSpeed = (speed: number) => {
    if (speed <= 0) return "0 Sektör/s"
    if (speed > 1000000) return `${(speed / 1000000).toFixed(2)} M Sektör/s`
    if (speed > 1000) return `${(speed / 1000).toFixed(2)} K Sektör/s`
    return `${Math.floor(speed)} Sektör/s`
  }

  const percent = progress.total > 0 ? Math.floor((progress.current / progress.total) * 100) : 0
  const isFinished = status === 'Tarama Tamamlandı' || status === 'Tarama İptal Edildi'
  const stopping = status === 'Durduruluyor...'

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
              Sürücü {driveIndex === -1 ? 'RAID' : driveIndex} taranıyor
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
            <span style={{ display: 'block', fontSize: '1.25rem', fontWeight: 600 }}>{formatTime(elapsed)}</span>
          </div>
          <div className="stat-pill" style={{ background: 'rgba(255,255,255,0.03)', padding: '12px 24px', borderRadius: '8px', textAlign: 'center', opacity: isFinished ? 0.3 : 1 }}>
            <span style={{ display: 'block', fontSize: '0.75rem', color: 'var(--text-muted)', textTransform: 'uppercase', letterSpacing: '0.05em' }}>Kalan Süre</span>
            <span style={{ display: 'block', fontSize: '1.25rem', fontWeight: 600, color: etaSeconds > 0 ? 'var(--accent-blue)' : 'inherit' }}>
              {isFinished ? '00:00:00' : (etaSeconds >= 0 ? formatTime(etaSeconds) : '...')}
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
        {scanType === 'deep' || scanType === 'full_carve'
          ? `Önce dosya sistemi metadata ($MFT vb.), sonra boş alan oyması (${carveSignatureCount != null ? carveSignatureCount.toLocaleString('tr-TR') : '…'} imza). Oymada sayı yavaş artar; metadata bitince kayıt sayısı zaten dolu olabilir. Thumbcache DB içindeki küçük önizlemeler de taranır.`
          : 'Hızlı tarama yalnız metadata okur (silinmiş MFT/FAT kayıtları dahil). Boş alan oyması için Derin tarama kullanın.'}
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
          <span>Sektör: {progress.current.toLocaleString()} / {progress.total ? progress.total.toLocaleString() : '?'}</span>
          <span style={{ color: 'var(--accent-blue)', fontFamily: 'monospace' }}>{formatSpeed(currentSpeed)}</span>
          <span>%{percent}</span>
        </div>
        <div style={{ width: '100%', height: '6px', background: 'rgba(255,255,255,0.1)', borderRadius: '3px', marginTop: '8px', overflow: 'hidden' }}>
          <div style={{ width: `${percent}%`, height: '100%', background: 'var(--accent-blue)', transition: 'width 0.3s ease' }}></div>
        </div>
      </div>

      <div className="scan-live-results glass-panel" style={{ flex: 1, display: 'flex', flexDirection: 'column', minHeight: '300px' }}>
        <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center', padding: 'var(--space-md) var(--space-xl)', borderBottom: '1px solid var(--panel-border)' }}>
          <h3 style={{ fontSize: '1rem', fontWeight: 500 }}>Silinmiş dosyalar (Sayfa {page + 1})</h3>
          <div style={{ display: 'flex', gap: 'var(--space-sm)' }}>
            <button className="btn-secondary" style={{ padding: '6px 12px' }} disabled={page === 0} onClick={() => setPage(p => p - 1)}>
              <ChevronLeft size={16} /> Önceki
            </button>
            <button className="btn-secondary" style={{ padding: '6px 12px' }} disabled={(page + 1) * limit >= listCount} onClick={() => setPage(p => p + 1)}>
              Sonraki <ChevronRight size={16} />
            </button>
          </div>
        </div>
        <div style={{ padding: 'var(--space-md)', overflowY: 'auto', flex: 1, display: 'flex', gap: 'var(--space-md)' }}>
          <div style={{ flex: 1, minWidth: 0 }}>
            {filesFound.length === 0 ? (
              <div style={{ textAlign: 'center', color: 'var(--text-muted)', marginTop: '2rem' }}>
                {totalFiles > 0
                  ? `Kayıt: ${totalFiles.toLocaleString('tr-TR')} (silinmiş: ${deletedCount.toLocaleString('tr-TR')})`
                  : 'Henüz dosya bulunamadı...'}
              </div>
            ) : (
              <div style={{ display: 'flex', flexDirection: 'column', gap: '4px' }}>
                {filesFound.map((f, i) => {
                  const isSelected = selectedFile && selectedFile.name === f.name && selectedFile.startSector === f.startSector
                  return (
                    <button
                      type="button"
                      key={i}
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
        {!isFinished && !stopping && (
          <button className="btn-danger" onClick={onStop} style={{ display: 'flex', alignItems: 'center', gap: '8px' }}>
            <Square size={16} fill="currentColor" /> Taramayı Durdur
          </button>
        )}
        {isFinished && (
          <button className="btn-primary" onClick={onViewResults}>
            Sonuçları Görüntüle
          </button>
        )}
        {isFinished && (
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
