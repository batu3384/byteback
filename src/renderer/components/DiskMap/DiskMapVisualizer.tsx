import React from 'react'
import './DiskMap.css'

interface DiskMapProps {
  totalSectors: number
  currentSector: number
  phase?: string
  filesFound?: number
  deletedCount?: number
}

const DiskMapVisualizer: React.FC<DiskMapProps> = ({
  totalSectors,
  currentSector,
  phase,
  filesFound = 0,
  deletedCount = 0,
}) => {
  const ratio = totalSectors > 0 ? Math.min(1, currentSector / totalSectors) : 0
  const pct = Math.floor(ratio * 100)
  const phaseLabel = phase === 'carve' ? 'Oyma (imza)' : 'Metadata'
  const countLabel =
    filesFound > 0
      ? `${phaseLabel} · ${filesFound.toLocaleString('tr-TR')} kayıt` +
        (deletedCount > 0 ? ` (silinmiş: ${deletedCount.toLocaleString('tr-TR')})` : '')
      : phaseLabel

  return (
    <div className="disk-map-container glass-panel">
      <div className="disk-map-header">
        <h3>Tarama ilerlemesi</h3>
        <span className="disk-map-phase">{countLabel}</span>
      </div>
      <div
        className="disk-map-bar"
        role="progressbar"
        aria-valuemin={0}
        aria-valuemax={100}
        aria-valuenow={pct}
        aria-label={countLabel}
      >
        <div className="disk-map-bar-fill" style={{ width: `${pct}%` }} />
      </div>
    </div>
  )
}

export default DiskMapVisualizer
