import React, { useEffect, useState } from 'react'
import type { DriveInfo, PartitionInfo, ScanOptions } from '../../../shared/types'
import type { ScanProfile } from '../../../shared/scan-profiles'
import { mediaNeedsTrimAck, smartTrimSignals } from '../../../shared/scan-profiles'
import SsdTrimModal from './SsdTrimModal'
import InlineAlert from '../InlineAlert'
import { useI18n, tFormat } from '../../i18n'
import './DriveCard.css'
import { HardDrive, Usb, Zap, Search, Binary, Activity, AlertTriangle } from 'lucide-react'

interface DriveCardProps {
  drive: DriveInfo
  onStartScan?: (driveIndex: number, scanType: string, scanOptions?: ScanOptions) => void
  onAction?: (page: any, data?: any) => void
  isAdmin?: boolean
  diskBusy?: boolean
}

function formatBytes(bytes: number): string {
  if (bytes === 0) return '0 B'
  const k = 1024
  const sizes = ['B', 'KB', 'MB', 'GB', 'TB']
  const i = Math.floor(Math.log(bytes) / Math.log(k))
  return parseFloat((bytes / Math.pow(k, i)).toFixed(2)) + ' ' + sizes[i]
}

function DriveCard({ drive, onStartScan, onAction, isAdmin, diskBusy }: DriveCardProps): React.ReactElement {
  const { t } = useI18n()
  const [partitions, setPartitions] = useState<PartitionInfo[]>([])
  const [partTableUnread, setPartTableUnread] = useState(false)
  const [partitionIndex, setPartitionIndex] = useState(-1)
  const [isSsd, setIsSsd] = useState(drive.type === 'SSD')
  const [smartUnread, setSmartUnread] = useState(false)
  const [pendingScan, setPendingScan] = useState<ScanProfile | null>(null)
  const [showTrimModal, setShowTrimModal] = useState(false)
  const [adminNotice, setAdminNotice] = useState<string | null>(null)

  useEffect(() => {
    if (diskBusy) return
    if (!window.api?.listPartitions) return
    let alive = true
    window.api.listPartitions(drive.index).then((list) => {
      if (!alive) return
      const unread = list.some((p) => p.type === 'table_unread')
      setPartTableUnread(unread)
      setPartitions(list.filter((p) => p.type !== 'table_unread'))
    }).catch((e: unknown) => {
      console.warn('[DriveCard] listPartitions failed', e)
      if (!alive) return
      setPartitions([])
      setPartTableUnread(false)
      setAdminNotice(t('drive.partTableError'))
    })
    return () => { alive = false }
  }, [drive.index, diskBusy])

  useEffect(() => {
    if (drive.type === 'SSD') {
      setIsSsd(true)
      setSmartUnread(false)
      return
    }
    if (!window.api?.getSmartStatus) {
      setSmartUnread(true)
      return
    }
    let alive = true
    window.api.getSmartStatus(drive.index).then((s) => {
      if (!alive) return
      const sig = smartTrimSignals(s)
      setIsSsd(sig.saysSsd)
      setSmartUnread(sig.unread)
    }).catch((e: unknown) => {
      console.warn('[DriveCard] getSmartStatus failed', e)
      if (!alive) return
      setSmartUnread(true)
    })
    return () => { alive = false }
  }, [drive.index, drive.type])

  const scanOptions = (): ScanOptions | undefined => {
    if (partitionIndex < 0) return undefined
    const p = partitions[partitionIndex]
    if (!p) return undefined
    return {
      partitionIndex,
      partitionStartSector: p.startSector,
      partitionSizeInSectors: p.sizeInSectors,
    }
  }

  const launchScan = (scanType: ScanProfile, extra?: ScanOptions) => {
    if (partitionIndex >= 0 && !partitions[partitionIndex]) {
      setAdminNotice(t('drive.partMissing'))
      return
    }
    onStartScan && onStartScan(drive.index, scanType, { ...scanOptions(), ...extra })
  }

  const requestScan = (scanType: ScanProfile) => {
    if (!isAdmin) {
      setAdminNotice(t('drive.adminRequired'))
      return
    }
    if (mediaNeedsTrimAck(scanType, drive.type, { unread: smartUnread, saysSsd: isSsd })) {
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
        pendingScan && mediaNeedsTrimAck(pendingScan, drive.type, { unread: smartUnread, saysSsd: isSsd })
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
        unknownMedia={smartUnread && !isSsd && drive.type !== 'SSD'}
        onConfirm={confirmTrim}
        onCancel={cancelTrim}
      />
      {adminNotice && (
        <div className="drive-admin-notice">
          <InlineAlert variant="warning" onDismiss={() => setAdminNotice(null)}>{adminNotice}</InlineAlert>
        </div>
      )}
      <div className="drive-card glass-panel">
        <div className="drive-card-header">
          <div className={`drive-icon-container${drive.type === 'SSD' || isSsd ? ' is-ssd' : ''}`}>
            {drive.type === 'USB' ? <Usb size={24} /> : <HardDrive size={24} />}
          </div>
          <div className="drive-title">
            <h3>
              {tFormat('drive.physical', { n: String(drive.index) })}
              <span className="drive-type-badge">{isSsd ? 'SSD' : drive.type}</span>
            </h3>
            <span className="drive-model">{drive.model || t('drive.unknownModel')}</span>
          </div>
        </div>
        
        <div className="drive-details">
          <div className="detail-row">
            <span className="detail-label">{t('drive.capacity')}</span>
            <span className="detail-value">{formatBytes(drive.sizeBytes)}</span>
          </div>
          <div className="detail-row">
            <span className="detail-label">{t('drive.sectorSize')}</span>
            <span className="detail-value">{drive.sectorSize} B</span>
          </div>
        </div>

        {isSsd && (
          <div className="drive-trim">
            <AlertTriangle size={14} /> {t('drive.trimWarning')}
          </div>
        )}
        {!isSsd && smartUnread && (
          <div className="drive-trim">
            <AlertTriangle size={14} /> {t('drive.trimUnread')}
          </div>
        )}
        {partTableUnread && (
          <InlineAlert variant="warning" testId="part-table-unread">{t('drive.partTableUnread')}</InlineAlert>
        )}

        {partitions.length > 0 && (
          <label className="drive-scope">
            <span className="drive-scope-label">{t('drive.scope')}</span>
            <select
              value={partitionIndex}
              onChange={(e) => setPartitionIndex(Number(e.target.value))}
              className="drive-scope-select"
            >
              <option value={-1}>{t('drive.wholeDisk')}</option>
              {partitions.map((p, i) => (
                <option key={i} value={i}>
                  {tFormat('drive.partition', { n: String(i + 1), type: p.type || 'unknown', sector: String(p.startSector), mib: String(Math.round(p.sizeInSectors * drive.sectorSize / (1024 * 1024))) })}
                </option>
              ))}
            </select>
          </label>
        )}

        <div className="drive-actions">
          <button 
            className="btn-primary" 
            type="button"
            disabled={!isAdmin || diskBusy}
            data-testid="scan-mode-quick"
            title={t('profile.quick.detail')}
            onClick={() => requestScan('quick')}
          >
            <Zap size={16} fill="currentColor" /> {t('profile.quick.label')}
          </button>
          <button 
            className="btn-secondary"
            type="button"
            disabled={!isAdmin || diskBusy}
            data-testid="scan-mode-deep"
            title={t('profile.deep.detail')}
            onClick={() => requestScan('deep')}
          >
            <Search size={16} /> {t('profile.deep.label')}
          </button>
          <button 
            className="btn-secondary"
            type="button"
            disabled={!isAdmin || diskBusy}
            data-testid="scan-mode-carve-only"
            title={t('profile.carve_only.detail')}
            onClick={() => requestScan('carve_only')}
          >
            <Binary size={16} /> {t('profile.carve_only.label')}
          </button>
          <button 
            className="btn-secondary"
            type="button"
            disabled={!isAdmin || diskBusy}
            data-testid="scan-mode-full-carve"
            title={t('profile.full_carve.detail')}
            onClick={() => requestScan('full_carve')}
          >
            <AlertTriangle size={16} /> {t('profile.full_carve.label')}
          </button>
          
          <div className="drive-actions-row">
            <button 
              className="btn-secondary"
              disabled={diskBusy}
              onClick={() => onAction && onAction('hex', { driveIndex: drive.index, sectorSize: drive.sectorSize })}
              title={diskBusy ? t('drive.hexBusyTitle') : t('drive.hexTitle')}
            >
              <Binary size={16} /> {t('drive.hex')}
            </button>
            <button 
              className="btn-secondary"
              disabled={diskBusy}
              onClick={() => onAction && onAction('smart', { driveIndex: drive.index })}
              title={diskBusy ? t('drive.smartBusyTitle') : t('drive.smartTitle')}
            >
              <Activity size={16} /> {t('drive.smart')}
            </button>
          </div>
        </div>
      </div>
    </>
  )
}

export default DriveCard
