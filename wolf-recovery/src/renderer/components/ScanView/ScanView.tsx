import React, { useEffect, useState, useRef } from 'react'
import './ScanView.css'
import DiskMapVisualizer from '../DiskMap/DiskMapVisualizer'
import { Search, CheckCircle, ChevronLeft, ChevronRight, File, Square } from 'lucide-react'

interface ScanViewProps {
  driveIndex: number | null
  scanType: string
  filesFound: any[]
  setFilesFound: React.Dispatch<React.SetStateAction<any[]>>
  progress: { current: number, total: number }
  status: string
  elapsed: number
  activeScanId: number
  onStop: () => void
  onCancel: () => void
  onViewResults: () => void
}

function ScanView({ 
  driveIndex, scanType, 
  filesFound, setFilesFound,
  progress, status,
  elapsed, activeScanId,
  onStop, onCancel, onViewResults 
}: ScanViewProps): React.ReactElement {
  
  const pollRef = useRef<ReturnType<typeof setInterval> | null>(null)
  const pageRef = useRef(0)
  const [page, setPage] = useState(0)
  const [totalFiles, setTotalFiles] = useState(0)
  const [selectedFile, setSelectedFile] = useState<any>(null)
  const limit = 50

  // Sliding Window ETA
  const speedHistoryRef = useRef<{ timestamp: number; sector: number }[]>([])
  const [currentSpeed, setCurrentSpeed] = useState<number>(0) // Sectors per second
  const [etaSeconds, setEtaSeconds] = useState<number>(-1)

  // Keep pageRef in sync
  useEffect(() => { pageRef.current = page }, [page])

  // ETA Calculation Effect
  useEffect(() => {
    if (status === 'Tarama Tamamlandı' || status === 'Tarama İptal Edildi') {
      setEtaSeconds(-1)
      setCurrentSpeed(0)
      return
    }

    const now = Date.now()
    const history = speedHistoryRef.current
    
    // Add current progress
    history.push({ timestamp: now, sector: progress.current })

    // Remove entries older than 5 seconds
    while (history.length > 0 && now - history[0].timestamp > 5000) {
      history.shift()
    }

    if (history.length > 1) {
      const first = history[0]
      const last = history[history.length - 1]
      const timeDiffSeconds = (last.timestamp - first.timestamp) / 1000
      
      if (timeDiffSeconds > 0) {
        const sectorDiff = last.sector - first.sector
        const speed = sectorDiff / timeDiffSeconds
        setCurrentSpeed(speed)
        
        if (speed > 0 && progress.total > progress.current) {
          const remainingSectors = progress.total - progress.current
          setEtaSeconds(remainingSectors / speed)
        } else {
          setEtaSeconds(-1)
        }
      }
    }
  }, [progress.current, status])

  // DB polling for paginated results
  useEffect(() => {
    if (driveIndex === null || activeScanId <= 0) return

    pollRef.current = setInterval(async () => {
      if (window.api && window.api.getFileCount && window.api.getFilesPage) {
        try {
          const count = await window.api.getFileCount(activeScanId)
          setTotalFiles(count)
          
          const pageData = await window.api.getFilesPage(activeScanId, pageRef.current * limit, limit)
          if (pageData && pageData.length > 0) {
            setFilesFound(prev => {
              // Create a Set of existing IDs for O(1) lookup
              const existingIds = new Set(prev.map(f => f.id));
              
              // Only add files that aren't already in the list
              const newFiles = pageData.filter((newFile: any) => !existingIds.has(newFile.id));
              
              if (newFiles.length === 0) return prev;
              
              return [...prev, ...newFiles];
            });
          }
        } catch (e) {
          console.error("Pagination error", e)
        }
      }
    }, 1500)

    return () => {
      if (pollRef.current) clearInterval(pollRef.current)
    }
  }, [driveIndex, activeScanId, setFilesFound])

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
            <h2 style={{ fontSize: '1.5rem', marginBottom: '4px' }}>Sürücü {driveIndex} Taranıyor</h2>
            <p style={{ color: 'var(--text-muted)' }}>{scanType === 'quick' ? 'Hızlı Tarama (MFT)' : 'Derin Tarama (Sektör)'} • {status}</p>
          </div>
        </div>
        <div className="scan-stats" style={{ display: 'flex', gap: 'var(--space-md)' }}>
          <div className="stat-pill" style={{ background: 'rgba(255,255,255,0.03)', padding: '12px 24px', borderRadius: '8px', textAlign: 'center' }}>
            <span style={{ display: 'block', fontSize: '0.75rem', color: 'var(--text-muted)', textTransform: 'uppercase', letterSpacing: '0.05em' }}>Bulunan</span>
            <span style={{ display: 'block', fontSize: '1.25rem', fontWeight: 600 }}>{totalFiles.toLocaleString()}</span>
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

      <div className="scan-progress-card glass-panel" style={{ padding: 'var(--space-xl)' }}>
        <DiskMapVisualizer 
          totalSectors={progress.total} 
          currentSector={progress.current} 
          badSectors={[]} 
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
          <h3 style={{ fontSize: '1rem', fontWeight: 500 }}>Bulunan Dosyalar (Sayfa {page + 1})</h3>
          <div style={{ display: 'flex', gap: 'var(--space-sm)' }}>
            <button className="btn-secondary" style={{ padding: '6px 12px' }} disabled={page === 0} onClick={() => setPage(p => p - 1)}>
              <ChevronLeft size={16} /> Önceki
            </button>
            <button className="btn-secondary" style={{ padding: '6px 12px' }} disabled={(page + 1) * limit >= totalFiles} onClick={() => setPage(p => p + 1)}>
              Sonraki <ChevronRight size={16} />
            </button>
          </div>
        </div>
        <div style={{ padding: 'var(--space-md)', overflowY: 'auto', flex: 1 }}>
          {filesFound.length === 0 ? (
            <div style={{ textAlign: 'center', color: 'var(--text-muted)', marginTop: '2rem' }}>Henüz dosya bulunamadı...</div>
          ) : (
            <div style={{ display: 'flex', flexDirection: 'column', gap: '4px' }}>
              {filesFound.map((f, i) => (
                <div key={i} onClick={() => setSelectedFile(f)} style={{ 
                  display: 'flex', alignItems: 'center', padding: '12px 16px', 
                  background: 'rgba(255,255,255,0.02)', borderRadius: '6px', cursor: 'pointer',
                  border: '1px solid transparent', transition: 'all 0.2s'
                }}
                onMouseEnter={(e) => e.currentTarget.style.borderColor = 'var(--panel-border)'}
                onMouseLeave={(e) => e.currentTarget.style.borderColor = 'transparent'}
                >
                  <File size={18} style={{ color: 'var(--accent-blue)', marginRight: '12px' }} />
                  <span style={{ fontWeight: 500 }}>{f.name}</span>
                  <span style={{ marginLeft: '16px', fontSize: '0.8rem', color: 'var(--text-muted)', background: 'rgba(255,255,255,0.05)', padding: '2px 8px', borderRadius: '12px' }}>{f.category}</span>
                  <span style={{ marginLeft: 'auto', color: 'var(--text-muted)', fontSize: '0.9rem' }}>
                    {(f.sizeBytes ? f.sizeBytes : f.size) ? ((f.sizeBytes || f.size) / 1024).toFixed(2) : 0} KB
                  </span>
                </div>
              ))}
            </div>
          )}
        </div>
      </div>

      <div className="scan-actions" style={{ display: 'flex', gap: 'var(--space-md)', justifyContent: 'flex-end', marginTop: 'var(--space-md)' }}>
        {!isFinished && (
          <button className="btn-danger" onClick={onStop} style={{ display: 'flex', alignItems: 'center', gap: '8px' }}>
            <Square size={16} fill="currentColor" /> Taramayı Durdur
          </button>
        )}
        {isFinished && (
          <button className="btn-primary" onClick={onViewResults}>
            Sonuçları Görüntüle
          </button>
        )}
        <button className="btn-secondary" onClick={onCancel}>İptal</button>
      </div>

    </div>
  )
}

export default ScanView
