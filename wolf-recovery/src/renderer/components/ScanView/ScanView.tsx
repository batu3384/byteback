import React, { useEffect, useState, useRef } from 'react'
import './ScanView.css'

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
        setFilesFound((prev) => [...prev, fileData])
      })
    }

    if (window.api && window.api.startScan) {
      window.api.startScan(driveIndex, scanType)
    }

    return () => {
      if (timerRef.current) clearInterval(timerRef.current)
      if (cleanupProgress) cleanupProgress()
      if (cleanupFileFound) cleanupFileFound()
    }
  }, [driveIndex, scanType])

  const handleStop = () => {
    if (window.api && window.api.stopScan) {
      window.api.stopScan()
    }
    if (timerRef.current) clearInterval(timerRef.current)
    setStatus('Tarama İptal Edildi')
  }

  const formatTime = (seconds: number) => {
    const h = Math.floor(seconds / 3600)
    const m = Math.floor((seconds % 3600) / 60)
    const s = seconds % 60
    return `${String(h).padStart(2, '0')}:${String(m).padStart(2, '0')}:${String(s).padStart(2, '0')}`
  }

  const percent = progress.total > 0 ? Math.floor((progress.current / progress.total) * 100) : 0
  const isFinished = status === 'Tarama Tamamlandı' || status === 'Tarama İptal Edildi'

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
        </div>
      </div>

      <div className="scan-progress-card glass-panel">
        <div className="progress-labels">
          <span>Sektör: {progress.current.toLocaleString()} / {progress.total ? progress.total.toLocaleString() : '?'}</span>
          <span>%{percent}</span>
        </div>
        <div className="progress-bar-bg">
          <div className="progress-bar-fill" style={{ width: `${percent}%` }}></div>
        </div>
      </div>

      <div className="scan-live-results glass-panel">
        <div className="live-header">
          <h3>Canlı Dosya Akışı</h3>
        </div>
        <div className="live-files">
          {filesFound.length === 0 ? (
            <div className="empty-files">Henüz dosya bulunamadı...</div>
          ) : (
            filesFound.slice(-20).reverse().map((f, i) => (
              <div key={i} className="live-file-item">
                <span className="file-icon">📄</span>
                <span className="file-name">{f.name}</span>
                <span className="file-size">{(f.size / 1024).toFixed(2)} KB</span>
              </div>
            ))
          )}
        </div>
      </div>

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

