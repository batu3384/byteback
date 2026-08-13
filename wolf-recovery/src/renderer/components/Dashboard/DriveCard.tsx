import React from 'react'
import type { DriveInfo } from '../../../shared/types'
import './DriveCard.css'
import { HardDrive, Usb, Zap, Search, Binary, Activity, ShieldAlert } from 'lucide-react'

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
      alert('Sektör düzeyinde tarama başlatmak için uygulamayı Yönetici olarak çalıştırmalısınız.')
      return
    }
    onStartScan && onStartScan(drive.index, scanType)
  }

  return (
    <div className="drive-card glass-panel" style={{ padding: '24px', display: 'flex', flexDirection: 'column', gap: '20px', transition: 'transform 0.2s ease, border-color 0.2s ease' }}>
      <div className="drive-card-header" style={{ display: 'flex', gap: '16px', alignItems: 'flex-start' }}>
        <div className="drive-icon-container" style={{ 
          background: drive.type === 'SSD' ? 'rgba(59, 130, 246, 0.1)' : 'rgba(255,255,255,0.05)', 
          padding: '16px', borderRadius: '12px',
          color: drive.type === 'SSD' ? 'var(--accent-blue)' : 'var(--text-main)'
        }}>
          {drive.type === 'USB' ? <Usb size={32} /> : <HardDrive size={32} />}
        </div>
        <div className="drive-title" style={{ flex: 1 }}>
          <h3 style={{ fontSize: '1.2rem', marginBottom: '4px', display: 'flex', alignItems: 'center', gap: '8px' }}>
            Fiziksel Sürücü {drive.index}
            <span className={`drive-type-badge`} style={{ 
              fontSize: '0.7rem', padding: '2px 8px', borderRadius: '4px',
              background: drive.type === 'SSD' ? 'rgba(59, 130, 246, 0.1)' : 'rgba(255,255,255,0.1)',
              color: drive.type === 'SSD' ? 'var(--accent-blue)' : 'var(--text-muted)',
              border: '1px solid currentColor',
              opacity: 0.8,
              fontWeight: 600
            }}>{drive.type}</span>
          </h3>
          <span className="drive-model" style={{ color: 'var(--text-muted)', fontSize: '0.9rem' }}>{drive.model || 'Bilinmeyen Model'}</span>
        </div>
      </div>
      
      <div className="drive-details" style={{ display: 'grid', gridTemplateColumns: '1fr 1fr', gap: '16px', background: 'rgba(0,0,0,0.2)', padding: '16px', borderRadius: '8px', border: '1px solid var(--panel-border)' }}>
        <div className="detail-row" style={{ display: 'flex', flexDirection: 'column', gap: '4px' }}>
          <span className="detail-label" style={{ fontSize: '0.8rem', color: 'var(--text-muted)', textTransform: 'uppercase' }}>Kapasite</span>
          <span className="detail-value highlight" style={{ fontSize: '1.2rem', fontWeight: 600, color: 'var(--text-main)' }}>{formatBytes(drive.sizeBytes)}</span>
        </div>
        <div className="detail-row" style={{ display: 'flex', flexDirection: 'column', gap: '4px' }}>
          <span className="detail-label" style={{ fontSize: '0.8rem', color: 'var(--text-muted)', textTransform: 'uppercase' }}>Sektör Boyutu</span>
          <span className="detail-value" style={{ fontSize: '1.2rem', fontWeight: 600 }}>{drive.sectorSize} B</span>
        </div>
      </div>

      <div className="drive-actions" style={{ display: 'flex', flexWrap: 'wrap', gap: '12px' }}>
        <button 
          className="btn-primary" 
          onClick={() => handleScan('quick')}
          style={{ flex: 1, padding: '12px', display: 'flex', justifyContent: 'center', gap: '8px' }}
        >
          <Zap size={18} fill="currentColor" /> Hızlı (MFT)
        </button>
        <button 
          className="btn-secondary" 
          onClick={() => handleScan('deep')}
          style={{ flex: 1, padding: '12px', display: 'flex', justifyContent: 'center', gap: '8px' }}
        >
          <Search size={18} /> Derin (Sektör)
        </button>
        
        <div style={{ width: '100%', display: 'flex', gap: '12px' }}>
          <button 
            className="btn-secondary" 
            onClick={() => onAction && onAction('hex', { driveIndex: drive.index, sectorSize: drive.sectorSize })}
            title="Sektörleri Hex formatında incele"
            style={{ flex: 1, fontSize: '0.85rem', padding: '8px', display: 'flex', justifyContent: 'center', gap: '6px' }}
          >
            <Binary size={16} /> Hex İncele
          </button>
          <button 
            className="btn-secondary" 
            onClick={() => onAction && onAction('smart', { driveIndex: drive.index })}
            title="S.M.A.R.T Sağlık Durumu"
            style={{ flex: 1, fontSize: '0.85rem', padding: '8px', display: 'flex', justifyContent: 'center', gap: '6px' }}
          >
            <Activity size={16} /> Sağlık Analizi
          </button>
        </div>
      </div>
    </div>
  )
}

export default DriveCard
