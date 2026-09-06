import React from 'react'
import './DiskMap.css'
import { useI18n, tFormat } from '../../i18n'

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
  const { t, lang } = useI18n()
  const locale = lang === 'tr' ? 'tr-TR' : 'en-US'
  const ratio = totalSectors > 0 ? Math.min(1, currentSector / totalSectors) : 0
  const pct = Math.floor(ratio * 100)
  const phaseLabel = t(phase === 'carve' || phase === 'carve_skipped' || phase === 'carve_only' ? 'scan.phase.carve' : 'scan.phase.metadata')
  const countLabel =
    filesFound > 0
      ? `${phaseLabel} · ${tFormat('diskmap.records', { n: filesFound.toLocaleString(locale) })}` +
        (deletedCount > 0 ? ` (${tFormat('diskmap.deleted', { n: deletedCount.toLocaleString(locale) })})` : '')
      : phaseLabel

  return (
    <div className="disk-map-container glass-panel">
      <div className="disk-map-header">
        <h3>{t('diskmap.title')}</h3>
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
