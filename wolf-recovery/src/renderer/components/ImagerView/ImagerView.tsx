import React, { useState, useEffect, useRef } from 'react'
import './ImagerView.css'

interface DriveInfo {
  index: number
  model: string
  serial: string
  sizeBytes: number
  sectorSize: number
  type: string
}

function ImagerView(): React.ReactElement {
  const [drives, setDrives] = useState<DriveInfo[]>([])
  const [selectedDrive, setSelectedDrive] = useState<number | ''>('')
  const [destPath, setDestPath] = useState<string>('')
  
  const [imaging, setImaging] = useState(false)
  const [progress, setProgress] = useState({ current: 0, total: 0 })
  const [status, setStatus] = useState<string>('')
  const [elapsed, setElapsed] = useState(0)
  
  const timerRef = useRef<NodeJS.Timeout | null>(null)

  useEffect(() => {
    // Load drives
    if (window.api && window.api.listDrives) {
      window.api.listDrives().then(setDrives)
    }

    let cleanupProgress: (() => void) | undefined
    if (window.api && window.api.onImagingProgress) {
      cleanupProgress = window.api.onImagingProgress((data: { current: number, total: number }) => {
        setProgress(data)
        if (data.current >= data.total && data.total > 0) {
          setStatus('İmaj Alma Tamamlandı ✅')
          setImaging(false)
          if (timerRef.current) clearInterval(timerRef.current)
        }
      })
    }

    return () => {
      if (cleanupProgress) cleanupProgress()
      if (timerRef.current) clearInterval(timerRef.current)
    }
  }, [])

  const handleStartImaging = () => {
    if (selectedDrive === '' || destPath.trim() === '') {
      alert('Lütfen kaynak sürücü ve hedef dosya yolu belirleyin.')
      return
    }
    
    setImaging(true)
    setStatus('İmaj Alınıyor...')
    setProgress({ current: 0, total: 0 })
    setElapsed(0)
    
    if (timerRef.current) clearInterval(timerRef.current)
    timerRef.current = setInterval(() => setElapsed(prev => prev + 1), 1000)
    
    if (window.api && window.api.startImaging) {
      window.api.startImaging(Number(selectedDrive), destPath)
    }
  }
  
  const handleStopImaging = () => {
    if (window.api && window.api.stopImaging) {
      window.api.stopImaging()
    }
    setImaging(false)
    setStatus('İmaj Alma İptal Edildi')
    if (timerRef.current) clearInterval(timerRef.current)
  }

  const formatTime = (seconds: number) => {
    const h = Math.floor(seconds / 3600)
    const m = Math.floor((seconds % 3600) / 60)
    const s = seconds % 60
    return `${String(h).padStart(2, '0')}:${String(m).padStart(2, '0')}:${String(s).padStart(2, '0')}`
  }

  const percent = progress.total > 0 ? Math.floor((progress.current / progress.total) * 100) : 0

  return (
    <div className="imager-view">
      <div className="imager-header glass-panel">
        <h2>Disk İmaj Alma (RAW / DD Kopyalama)</h2>
        <p>Seçilen diskin donanım seviyesinde sektör-sektör kopyasını alır. Yeni native C++ motoru üzerinden 0 veri kaybı garantisi.</p>
      </div>

      <div className="imager-content glass-panel">
        <div className="form-group">
          <label>Kaynak Sürücü</label>
          <select 
            className="form-select" 
            value={selectedDrive} 
            onChange={(e) => setSelectedDrive(e.target.value === '' ? '' : Number(e.target.value))}
            disabled={imaging}
          >
            <option value="">Sürücü Seçin...</option>
            {drives.map(d => (
              <option key={d.index} value={d.index}>
                Sürücü {d.index} - {d.model} ({Math.floor(d.sizeBytes / (1024*1024*1024))} GB)
              </option>
            ))}
          </select>
        </div>

        <div className="form-group">
          <label>İmaj Formatı</label>
          <select className="form-select" disabled={imaging}>
            <option>RAW (DD) Birebir Kopya (.dd, .img)</option>
          </select>
        </div>

        <div className="form-group">
          <label>Hedef Dizin (Tam Yol: Örn: D:\Kopya.dd)</label>
          <div className="path-input-group">
            <input 
              type="text" 
              className="form-input" 
              placeholder="D:\Recovered_Image.dd" 
              value={destPath}
              onChange={(e) => setDestPath(e.target.value)}
              disabled={imaging}
            />
          </div>
        </div>

        <div className="form-actions">
          {!imaging ? (
            <button className="btn-primary start-btn" onClick={handleStartImaging}>
              İmaj Almayı Başlat
            </button>
          ) : (
            <button className="btn-secondary stop-btn" onClick={handleStopImaging}>
              İptal Et
            </button>
          )}
        </div>

        {(imaging || status) && (
          <div className="imager-progress-card glass-panel" style={{ marginTop: '20px', padding: '15px' }}>
            <div style={{ display: 'flex', justifyContent: 'space-between', marginBottom: '10px' }}>
              <span style={{ fontWeight: 'bold', color: 'var(--neon-cyan)' }}>{status}</span>
              <span style={{ color: 'var(--text-muted)' }}>Süre: {formatTime(elapsed)}</span>
            </div>
            
            <div className="progress-labels">
              <span>Sektör: {progress.current.toLocaleString()} / {progress.total ? progress.total.toLocaleString() : '?'}</span>
              <span>%{percent}</span>
            </div>
            <div className="progress-bar-bg" style={{ marginTop: '5px' }}>
              <div className="progress-bar-fill" style={{ width: `${percent}%` }}></div>
            </div>
          </div>
        )}
      </div>
    </div>
  )
}

export default ImagerView
