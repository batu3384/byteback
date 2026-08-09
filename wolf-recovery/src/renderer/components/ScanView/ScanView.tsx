import React, { useEffect, useState, useRef } from 'react'
import './ScanView.css'

interface ScanViewProps {
  driveIndex: number | null
  scanType: string
  onCancel: () => void
  onViewResults?: () => void
}

interface FoundFile {
  name: string
  size: number
}

function ScanView({ driveIndex, scanType, onCancel, onViewResults }: ScanViewProps): React.ReactElement {
  const [progress, setProgress] = useState(0)
  const [currentSector, setCurrentSector] = useState(0)
  const [totalSectors, setTotalSectors] = useState(1)
  const [foundFiles, setFoundFiles] = useState<FoundFile[]>([])
  const [isScanning, setIsScanning] = useState(false)
  
  const filesEndRef = useRef<HTMLDivElement>(null)

  useEffect(() => {
    if (driveIndex === null) return
    
    setIsScanning(true)
    
    window.api.onScanProgress((data) => {
      setCurrentSector(data.current)
      setTotalSectors(data.total)
      setProgress(Math.floor((data.current / data.total) * 100))
      
      if (data.current >= data.total) {
        setIsScanning(false)
      }
    })

    window.api.onScanFileFound((data) => {
      setFoundFiles(prev => [...prev, data])
    })

    window.api.startScan(driveIndex, scanType)

    return () => {
      window.api.removeAllScanListeners()
    }
  }, [driveIndex, scanType])

  useEffect(() => {
    filesEndRef.current?.scrollIntoView({ behavior: 'smooth' })
  }, [foundFiles])

  const handleStop = () => {
    window.api.stopScan()
    setIsScanning(false)
  }

  const formatSize = (bytes: number) => {
    if (bytes < 1024) return bytes + ' B'
    if (bytes < 1024 * 1024) return (bytes / 1024).toFixed(2) + ' KB'
    if (bytes < 1024 * 1024 * 1024) return (bytes / (1024 * 1024)).toFixed(2) + ' MB'
    return (bytes / (1024 * 1024 * 1024)).toFixed(2) + ' GB'
  }

  return (
    <div className="scan-view">
      <div className="scan-header">
        <h2>{scanType === 'quick' ? 'Quick Scan' : 'Deep Scan'} 🐺</h2>
        <p className="scan-status">
          {isScanning ? 'Scanning...' : 'Scan Complete'}
        </p>
      </div>

      <div className="scan-progress-container">
        <div className="progress-bar-bg">
          <div 
            className={`progress-bar-fill ${isScanning ? 'pulse' : ''}`}
            style={{ width: `${progress}%` }}
          ></div>
        </div>
        <div className="progress-stats">
          <span>{progress}% Complete</span>
          <span>Sectors: {currentSector.toLocaleString()} / {totalSectors.toLocaleString()}</span>
        </div>
      </div>

      <div className="scan-actions">
        {isScanning ? (
          <button className="btn-danger" onClick={handleStop}>Stop Scan</button>
        ) : (
          <button className="btn-primary" onClick={onViewResults || onCancel}>View Results</button>
        )}
      </div>

      <div className="found-files-panel">
        <h3>Recovered Files ({foundFiles.length})</h3>
        <div className="files-list">
          {foundFiles.length === 0 ? (
            <div className="empty-files">No files found yet...</div>
          ) : (
            foundFiles.map((f, i) => (
              <div key={i} className="file-item slide-in">
                <span className="file-icon">📄</span>
                <span className="file-name">{f.name}</span>
                <span className="file-size">{formatSize(f.size)}</span>
              </div>
            ))
          )}
          <div ref={filesEndRef} />
        </div>
      </div>
    </div>
  )
}

export default ScanView

