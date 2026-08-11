import React from 'react'
import type { DriveInfo } from '../../../shared/types'
import './DriveCard.css'

interface DriveCardProps {
  drive: DriveInfo
  onStartScan?: (driveIndex: number, scanType: string) => void
  onAction?: (page: any, data?: any) => void
  isAdmin?: boolean
}

function formatBytes(bytes: number): string {
  if (bytes === 0) return '0 B'
  const k = 1024
  const sizes = ['B', 'KB', 'MB', 'GB', 'TB']
  const i = Math.floor(Math.log(bytes) / Math.log(k))
  return parseFloat((bytes / Math.pow(k, i)).toFixed(2)) + ' ' + sizes[i]
}

function DriveCard({ drive, onStartScan, onAction, isAdmin }: DriveCardProps): React.ReactElement {
  const handleScan = (scanType: string) => {
    if (!isAdmin) {
      alert('Tarama başlatmak için uygulamayı Yönetici olarak çalıştırmalısınız.')
      return
    }
    onStartScan && onStartScan(drive.index, scanType)
  }

  return (
    <div className="drive-card glass-panel">
      <div className="drive-card-header">
        <div className="drive-icon-container">
          <span className="drive-icon">{drive.type === 'SSD' ? '⚡' : drive.type === 'USB' ? '🔌' : '💽'}</span>
        </div>
        <div className="drive-title">
          <h3>Fiziksel Sürücü {drive.index}</h3>
          <span className="drive-model">{drive.model || 'Bilinmeyen Model'}</span>
        </div>
        <span className={`drive-type-badge ${drive.type.toLowerCase()}`}>{drive.type}</span>
      </div>
      
      <div className="drive-details">
        <div className="detail-row">
          <span className="detail-label">Seri No</span>
          <span className="detail-value">{drive.serial || 'N/A'}</span>
        </div>
        <div className="detail-row">
          <span className="detail-label">Kapasite</span>
          <span className="detail-value highlight">{formatBytes(drive.sizeBytes)}</span>
        </div>
        <div className="detail-row">
          <span className="detail-label">Sektör Boyutu</span>
          <span className="detail-value">{drive.sectorSize} Bayt</span>
        </div>
      </div>

      <div className="drive-actions">
        <button 
          className="btn-primary" 
          onClick={() => handleScan('quick')}
        >
          <span className="btn-icon">⚡</span> Hızlı Tarama
        </button>
        <button 
          className="btn-accent" 
          onClick={() => handleScan('deep')}
        >
          <span className="btn-icon">🧬</span> Derin Tarama
        </button>
        <button 
          className="btn-secondary" 
          onClick={() => onAction && onAction('hex', { driveIndex: drive.index, sectorSize: drive.sectorSize })}
          title="Sektörleri Hex formatında incele"
        >
          Hex
        </button>
        <button 
          className="btn-secondary" 
          onClick={() => onAction && onAction('smart', { driveIndex: drive.index })}
          title="SMART Sağlık Durumu"
        >
          SMART
        </button>
      </div>
    </div>
  )
}

export default DriveCard

