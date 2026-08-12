import React, { useEffect, useState, useRef } from 'react'
import './ScanView.css'
import DiskMapVisualizer from '../DiskMap/DiskMapVisualizer'

interface ScanViewProps {
  driveIndex: number | null
  scanType: string
  filesFound: any[]
  setFilesFound: React.Dispatch<React.SetStateAction<any[]>>
  progress: { current: number, total: number }
  setProgress: React.Dispatch<React.SetStateAction<{ current: number, total: number }>>
  status: string
  setStatus: React.Dispatch<React.SetStateAction<string>>
  elapsed: number
  setElapsed: React.Dispatch<React.SetStateAction<number>>
  onCancel: () => void
  onViewResults: () => void
}

function ScanView({ 
  driveIndex, scanType, 
  filesFound, setFilesFound,
  progress, setProgress,
  status, setStatus,
  elapsed, setElapsed,
  onCancel, onViewResults 
}: ScanViewProps): React.ReactElement {
  const timerRef = useRef<ReturnType<typeof setInterval> | null>(null)
  const pollRef = useRef<ReturnType<typeof setInterval> | null>(null)
  const [page, setPage] = useState(0)
  const [totalFiles, setTotalFiles] = useState(0)
  const [selectedFile, setSelectedFile] = useState<any>(null)
  const limit = 50

  useEffect(() => {
    if (driveIndex === null) return

    // Start elapsed timer
    timerRef.current = setInterval(() => {
      setElapsed(prev => prev + 1)
    }, 1000)

    let cleanupProgress: (() => void) | undefined
    let cleanupFileFound: (() => void) | undefined

    if (window.api && window.api.onScanProgress) {
      cleanupProgress = window.api.onScanProgress((data: { current: number, total: number }) => {
        setProgress(data)
        if (data.current >= data.total && data.total > 0) {
          setStatus('Tarama Tamamlandı')
          if (timerRef.current) clearInterval(timerRef.current)
        }
      })
    }

    if (window.api && window.api.onScanFileFound) {
      cleanupFileFound = window.api.onScanFileFound((fileData: { name: string, size: number }) => {
        // We still listen to live events for quick UI updates but real data comes from DB polling
        setTotalFiles(prev => prev + 1)
      })
    }

    if (window.api && window.api.startScan) {
      window.api.startScan(driveIndex, scanType)
    }

    // Polling DB for pagination data every second
    pollRef.current = setInterval(async () => {
      if (window.api && window.api.getFileCount && window.api.getFilesPage) {
        try {
          const scanId = 1 // Assuming first scan for now
          const count = await window.api.getFileCount(scanId)
          setTotalFiles(count)
          
          const pageData = await window.api.getFilesPage(scanId, page * limit, limit)
          if (pageData && pageData.length > 0) {
            setFilesFound(pageData)
          }
        } catch (e) {
          console.error("Pagination error", e)
        }
      }
    }, 1500)

    return () => {
      if (timerRef.current) clearInterval(timerRef.current)
      if (pollRef.current) clearInterval(pollRef.current)
      if (cleanupProgress) cleanupProgress()
      if (cleanupFileFound) cleanupFileFound()
    }
  }, [driveIndex, scanType, page])

  const handleStop = () => {
    if (window.api && window.api.stopScan) {
      window.api.stopScan()
    }
    if (timerRef.current) clearInterval(timerRef.current)
    setStatus('Tarama İptal Edildi')
  }

  const formatTime = (seconds: number) => {
    if (seconds < 0) return "--:--:--";
    const h = Math.floor(seconds / 3600)
    const m = Math.floor((seconds % 3600) / 60)
    const s = Math.floor(seconds % 60)
    return `${String(h).padStart(2, '0')}:${String(m).padStart(2, '0')}:${String(s).padStart(2, '0')}`
  }

  const percent = progress.total > 0 ? Math.floor((progress.current / progress.total) * 100) : 0
  const isFinished = status === 'Tarama Tamamlandı' || status === 'Tarama İptal Edildi'

  // ETA Calculation
  let etaSeconds = -1;
  if (!isFinished && elapsed > 5 && progress.current > 0 && progress.total > 0) {
    const sectorsPerSecond = progress.current / elapsed;
    const remainingSectors = progress.total - progress.current;
    etaSeconds = remainingSectors / sectorsPerSecond;
  }

  return (
    <div className="scan-view">
      <div className="scan-header glass-panel">
        <div className="scan-info">
          <div className={`scan-icon ${isFinished ? '' : 'spinner'}`}>{isFinished ? '✅' : '🔍'}</div>
          <div>
            <h2>Sürücü {driveIndex} Taranıyor</h2>
            <p>{scanType === 'quick' ? 'Hızlı Tarama (MFT Kayıtları)' : 'Derin Tarama (Sektör Bazlı)'} • {status}</p>
          </div>
        </div>
        <div className="scan-stats">
          <div className="stat-pill">
            <span className="label">Bulunan</span>
            <span className="value">{filesFound.length}</span>
          </div>
          <div className="stat-pill">
            <span className="label">Geçen Süre</span>
            <span className="value">{formatTime(elapsed)}</span>
          </div>
          <div className="stat-pill" style={{ opacity: isFinished ? 0.3 : 1 }}>
            <span className="label">Kalan Süre (ETA)</span>
            <span className="value" style={{ color: etaSeconds > 0 ? 'var(--neon-cyan)' : 'inherit' }}>
              {isFinished ? '00:00:00' : (etaSeconds >= 0 ? formatTime(etaSeconds) : 'Hesaplanıyor...')}
            </span>
          </div>
        </div>
      </div>

      <div className="scan-progress-card glass-panel">
        <DiskMapVisualizer 
          totalSectors={progress.total} 
          currentSector={progress.current} 
          badSectors={[]} 
        />
        <div className="progress-labels" style={{ marginTop: '16px' }}>
          <span>Sektör: {progress.current.toLocaleString()} / {progress.total ? progress.total.toLocaleString() : '?'}</span>
          <span>%{percent}</span>
        </div>
        <div className="progress-bar-bg">
          <div className="progress-bar-fill" style={{ width: `${percent}%` }}></div>
        </div>
      </div>

      <div className="scan-live-results glass-panel">
        <div className="live-header" style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center' }}>
          <h3>Bulunan Dosyalar (Sayfa {page + 1})</h3>
          <div className="pagination-controls">
            <button 
              className="btn-secondary" 
              style={{ padding: '4px 8px', marginRight: '5px', fontSize: '12px' }}
              disabled={page === 0} 
              onClick={() => setPage(p => p - 1)}
            >
              ◀ Önceki
            </button>
            <button 
              className="btn-secondary"
              style={{ padding: '4px 8px', fontSize: '12px' }}
              disabled={(page + 1) * limit >= totalFiles}
              onClick={() => setPage(p => p + 1)}
            >
              Sonraki ▶
            </button>
          </div>
        </div>
        <div className="live-files">
          {filesFound.length === 0 ? (
            <div className="empty-files">Henüz dosya bulunamadı...</div>
          ) : (
            filesFound.map((f, i) => (
              <div key={i} className="live-file-item" onClick={() => setSelectedFile(f)} style={{ cursor: 'pointer' }}>
                <span className="file-icon">📄</span>
                <span className="file-name">{f.name}</span>
                <span className="file-category" style={{ opacity: 0.6, fontSize: '0.8em', marginLeft: '10px' }}>{f.category}</span>
                <span className="file-size" style={{ marginLeft: 'auto' }}>{(f.sizeBytes ? f.sizeBytes : f.size) ? ((f.sizeBytes || f.size) / 1024).toFixed(2) : 0} KB</span>
                <span className="file-preview-btn" style={{ marginLeft: '15px', color: 'var(--neon-cyan)', fontSize: '0.85rem' }}>👁️ Preview</span>
              </div>
            ))
          )}
        </div>
      </div>

      {selectedFile && (
        <div className="preview-modal-overlay" onClick={() => setSelectedFile(null)} style={{ position: 'absolute', top: 0, left: 0, right: 0, bottom: 0, backgroundColor: 'rgba(0,0,0,0.7)', zIndex: 100, display: 'flex', alignItems: 'center', justifyContent: 'center' }}>
          <div className="preview-modal glass-panel" onClick={(e) => e.stopPropagation()} style={{ width: '600px', maxWidth: '90%', padding: '20px', borderRadius: '12px' }}>
            <h3 style={{ color: 'var(--neon-cyan)', marginBottom: '10px' }}>Safe File Preview</h3>
            <p><strong>Name:</strong> {selectedFile.name}</p>
            <p><strong>Size:</strong> {(selectedFile.sizeBytes || selectedFile.size) / 1024} KB</p>
            <p><strong>Status:</strong> {selectedFile.status === 0 ? 'Intact' : 'Partially Overwritten'}</p>
            <div className="preview-hex-box" style={{ marginTop: '15px', backgroundColor: '#0B0F19', padding: '10px', fontFamily: 'monospace', fontSize: '0.85rem', color: '#00E676', border: '1px solid var(--border-subtle)', borderRadius: '4px', height: '150px', overflowY: 'auto' }}>
              4D 5A 90 00 03 00 00 00 04 00 00 00 FF FF 00 00 <br/>
              B8 00 00 00 00 00 00 00 40 00 00 00 00 00 00 00 <br/>
              00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 <br/>
              ... (Safe Hex View - Payload Exec Prevented) ...
            </div>
            <button className="btn-secondary" style={{ marginTop: '20px', width: '100%' }} onClick={() => setSelectedFile(null)}>Close Preview</button>
          </div>
        </div>
      )}

      <div className="scan-actions">
        {isFinished ? (
          <button className="btn-primary" onClick={onViewResults}>Sonuçları Görüntüle</button>
        ) : (
          <button className="btn-secondary" onClick={handleStop}>Durdur</button>
        )}
        <button className="btn-secondary" onClick={onCancel}>Geri Dön</button>
      </div>
    </div>
  )
}

export default ScanView

