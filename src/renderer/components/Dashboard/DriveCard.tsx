import React, { useEffect, useState } from 'react'
import type { DriveInfo, PartitionInfo, ScanOptions } from '../../../shared/types'
import type { ScanProfile } from '../../../shared/scan-profiles'
import { SCAN_PROFILES } from '../../../shared/scan-profiles'
import SsdTrimModal from './SsdTrimModal'
import InlineAlert from '../InlineAlert'
import './DriveCard.css'
import { HardDrive, Usb, Zap, Search, Binary, Activity, AlertTriangle } from 'lucide-react'

interface DriveCardProps {
  drive: DriveInfo
  onStartScan?: (driveIndex: number, scanType: string, scanOptions?: ScanOptions) => void
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
  const [partitions, setPartitions] = useState<PartitionInfo[]>([])
  const [partitionIndex, setPartitionIndex] = useState(-1)
  const [isSsd, setIsSsd] = useState(drive.type === 'SSD')
  const [pendingScan, setPendingScan] = useState<ScanProfile | null>(null)
  const [showTrimModal, setShowTrimModal] = useState(false)
  const [adminNotice, setAdminNotice] = useState<string | null>(null)

  useEffect(() => {
    if (!window.api?.listPartitions) return
    window.api.listPartitions(drive.index).then(setPartitions).catch(() => setPartitions([]))
  }, [drive.index])

  useEffect(() => {
    if (drive.type === 'SSD') {
      setIsSsd(true)
      return
    }
    if (!window.api?.getSmartStatus) return
    window.api.getSmartStatus(drive.index).then((s) => {
      if (s.isValid && s.isSsd) setIsSsd(true)
    }).catch(() => {})
  }, [drive.index, drive.type])

  const scanOptions = (): ScanOptions | undefined => {
    if (partitionIndex < 0) return undefined
    const p = partitions[partitionIndex]
    if (!p) return { partitionIndex }
    return {
      partitionIndex,
      partitionStartSector: p.startSector,
      partitionSizeInSectors: p.sizeInSectors,
    }
  }

  const launchScan = (scanType: ScanProfile, extra?: ScanOptions) => {
    onStartScan && onStartScan(drive.index, scanType, { ...scanOptions(), ...extra })
  }

  const requestScan = (scanType: ScanProfile) => {
    if (!isAdmin) {
      setAdminNotice('Sektör düzeyinde tarama başlatmak için uygulamayı Yönetici olarak çalıştırmalısınız.')
      return
    }
    if (isSsd) {
      setPendingScan(scanType)
      setShowTrimModal(true)
      return
    }
    launchScan(scanType)
  }

  const confirmTrim = () => {
    setShowTrimModal(false)
    if (pendingScan) {
      const extra =
        pendingScan === 'deep' || pendingScan === 'full_carve'
          ? { allowSsdDeepScan: true }
          : undefined
      launchScan(pendingScan, extra)
    }
    setPendingScan(null)
  }

  const cancelTrim = () => {
    setShowTrimModal(false)
    setPendingScan(null)
  }

  return (
    <>
      <SsdTrimModal
        open={showTrimModal}
        scanType={pendingScan ?? 'deep'}
        onConfirm={confirmTrim}
        onCancel={cancelTrim}
      />
      {adminNotice && (
        <div style={{ marginBottom: '8px' }}>
          <InlineAlert variant="warning" onDismiss={() => setAdminNotice(null)}>{adminNotice}</InlineAlert>
        </div>
      )}
      <div className="drive-card glass-panel" style={{ padding: '24px', display: 'flex', flexDirection: 'column', gap: '20px', transition: 'transform 0.2s ease, border-color 0.2s ease' }}>
        <div className="drive-card-header" style={{ display: 'flex', gap: '16px', alignItems: 'flex-start' }}>
          <div className="drive-icon-container" style={{ 
            background: drive.type === 'SSD' || isSsd ? 'rgba(59, 130, 246, 0.1)' : 'rgba(255,255,255,0.05)', 
            padding: '16px', borderRadius: '12px',
            color: drive.type === 'SSD' || isSsd ? 'var(--accent-blue)' : 'var(--text-main)'
          }}>
            {drive.type === 'USB' ? <Usb size={32} /> : <HardDrive size={32} />}
          </div>
          <div className="drive-title" style={{ flex: 1 }}>
            <h3 style={{ fontSize: '1.2rem', marginBottom: '4px', display: 'flex', alignItems: 'center', gap: '8px' }}>
              Fiziksel Sürücü {drive.index}
              <span className={`drive-type-badge`} style={{ 
                fontSize: '0.7rem', padding: '2px 8px', borderRadius: '4px',
                background: (drive.type === 'SSD' || isSsd) ? 'rgba(59, 130, 246, 0.1)' : 'rgba(255,255,255,0.1)',
                color: (drive.type === 'SSD' || isSsd) ? 'var(--accent-blue)' : 'var(--text-muted)',
                border: '1px solid currentColor',
                opacity: 0.8,
                fontWeight: 600
              }}>{isSsd ? 'SSD' : drive.type}</span>
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

        {isSsd && (
          <div style={{ fontSize: '0.8rem', color: 'var(--warning-yellow)', display: 'flex', gap: '6px', alignItems: 'center' }}>
            <AlertTriangle size={14} /> SSD — tarama öncesi TRIM uyarısı gösterilir
          </div>
        )}

        {partitions.length > 0 && (
          <label style={{ display: 'flex', flexDirection: 'column', gap: '6px', fontSize: '0.85rem' }}>
            <span style={{ color: 'var(--text-muted)' }}>Tarama kapsamı</span>
            <select
              value={partitionIndex}
              onChange={(e) => setPartitionIndex(Number(e.target.value))}
              style={{ padding: '8px', borderRadius: '6px', background: 'rgba(0,0,0,0.25)', color: 'var(--text-main)', border: '1px solid var(--panel-border)' }}
            >
              <option value={-1}>Tüm fiziksel disk</option>
              {partitions.map((p, i) => (
                <option key={i} value={i}>
                  Bölüm {i + 1} — {p.type || 'unknown'} @ sektör {p.startSector} ({Math.round(p.sizeInSectors * drive.sectorSize / (1024 * 1024))} MiB)
                </option>
              ))}
            </select>
          </label>
        )}

        <div className="drive-actions" style={{ display: 'flex', flexDirection: 'column', gap: '10px' }}>
          <button 
            className="btn-primary" 
            type="button"
            data-testid="scan-mode-quick"
            title={SCAN_PROFILES.quick.detail}
            onClick={() => requestScan('quick')}
            style={{ padding: '12px', display: 'flex', justifyContent: 'center', gap: '8px' }}
          >
            <Zap size={18} fill="currentColor" /> {SCAN_PROFILES.quick.label}
          </button>
          <button 
            className="btn-secondary"
            type="button"
            data-testid="scan-mode-deep"
            title={SCAN_PROFILES.deep.detail}
            onClick={() => requestScan('deep')}
            style={{ padding: '12px', display: 'flex', justifyContent: 'center', gap: '8px' }}
          >
            <Search size={18} /> {SCAN_PROFILES.deep.label}
          </button>
          <button 
            className="btn-secondary"
            type="button"
            data-testid="scan-mode-full-carve"
            title={SCAN_PROFILES.full_carve.detail}
            onClick={() => requestScan('full_carve')}
            style={{ padding: '12px', display: 'flex', justifyContent: 'center', gap: '8px', borderColor: 'var(--warning-yellow)' }}
          >
            <AlertTriangle size={16} /> {SCAN_PROFILES.full_carve.label}
          </button>
          
          <div style={{ display: 'flex', gap: '12px' }}>
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
    </>
  )
}

export default DriveCard
