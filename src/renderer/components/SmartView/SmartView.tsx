import React, { useEffect, useRef, useState } from 'react'
import './SmartView.css'
import { Activity, HardDrive, Thermometer, Clock, AlertTriangle, ShieldCheck, RefreshCw, Zap, Database, Power } from 'lucide-react'
import type { SmartStatus } from '../../../shared/types'
import { useI18n, tFormat } from '../../i18n'

interface SmartViewProps {
  driveIndex?: number | null
}

function formatBytes(bytes: number): string {
  if (!bytes || bytes <= 0) return '—'
  if (bytes < 1024) return bytes + ' B'
  if (bytes < 1024 ** 2) return (bytes / 1024).toFixed(2) + ' KB'
  if (bytes < 1024 ** 3) return (bytes / 1024 ** 2).toFixed(2) + ' MB'
  if (bytes < 1024 ** 4) return (bytes / 1024 ** 3).toFixed(2) + ' GB'
  return (bytes / 1024 ** 4).toFixed(2) + ' TB'
}

function SmartView({ driveIndex }: SmartViewProps): React.ReactElement {
  const { t } = useI18n()
  const [smartData, setSmartData] = useState<SmartStatus | null>(null)
  const [loading, setLoading] = useState<boolean>(false)
  // Stale-response guard: navigating away mid-read must not setState.
  // Setup re-arms the flag so StrictMode's double mount stays live.
  const aliveRef = useRef(true)
  useEffect(() => {
    aliveRef.current = true
    return () => { aliveRef.current = false }
  }, [])

  useEffect(() => {
    if (driveIndex !== undefined && driveIndex !== null) {
      fetchSmartData(driveIndex)
    }
  }, [driveIndex])

  const fetchSmartData = async (index: number) => {
    setLoading(true)
    try {
      if (window.api && window.api.getSmartStatus) {
        const data = await window.api.getSmartStatus(index)
        if (aliveRef.current) setSmartData(data)
      }
    } catch (err) {
      console.error(err)
      // Drop stale readings so the read-failed panel becomes the visible
      // error surface instead of silently showing old data.
      if (aliveRef.current) setSmartData(null)
    } finally {
      if (aliveRef.current) setLoading(false)
    }
  }

  const isHealthy = smartData && smartData.healthScore === 'Good'
  const hasWarnings = smartData && ((smartData.reallocatedSectors ?? 0) > 0 || (smartData.pendingSectors ?? 0) > 0)
  const healthTone = isHealthy && !hasWarnings ? 'ok' : hasWarnings ? 'warn' : 'crit'

  if (driveIndex === undefined || driveIndex === null) {
    return (
      <div className="smart-view empty glass-panel examiner-empty" data-testid="smart-empty" role="status">
        <div className="examiner-icon neutral" aria-hidden="true">
          <Activity size={28} color="var(--text-main)" />
        </div>
        <h3>{t('smart.emptyTitle')}</h3>
        <p>{t('smart.noDriveBody')}</p>
      </div>
    )
  }

  return (
    <div className="smart-view">
      <div className="smart-header glass-panel">
        <div className="smart-header-info">
          <div className="examiner-icon">
            <Activity size={32} color="var(--accent-blue)" />
          </div>
          <div>
            <h2>{t('smart.title')}</h2>
            <p>{tFormat('smart.subtitle', { n: String(driveIndex) })}</p>
          </div>
        </div>
        <button className="btn-secondary" onClick={() => fetchSmartData(driveIndex)} disabled={loading}>
          <RefreshCw size={16} className={loading ? 'spinner' : ''} aria-hidden="true" /> {t('dash.refresh')}
        </button>
      </div>

      {loading ? (
        <div className="loading-state glass-panel examiner-empty" role="status">
          <RefreshCw size={32} className="spinner" />
          <p>{t('smart.loading')}</p>
        </div>
      ) : smartData && smartData.isValid ? (
        <div className="smart-content">

          <div className={`smart-overview glass-panel ${healthTone}`}>
            {healthTone === 'ok' ? <ShieldCheck size={32} color="var(--success-green)" /> : <AlertTriangle size={32} color={healthTone === 'warn' ? 'var(--warning-yellow)' : 'var(--alert-red)'} />}
            <div>
              <h3>
                {healthTone === 'ok' ? t('smart.okTitle') : healthTone === 'warn' ? t('smart.warnTitle') : t('smart.critTitle')}
              </h3>
              <p>
                {tFormat('smart.model', { m: smartData.driveModel || t('drive.unknownModel') })}
                {smartData.isNvme ? ' · NVMe' : ''}
                {smartData.isSsd ? ' · SSD' : ''}
              </p>
            </div>
            <div className={`smart-score ${healthTone}`}>
              {smartData.healthScore || t('smart.na')}
            </div>
          </div>
          {!smartData.isNvme && (
            <p className="smart-ata-note">
              {t('smart.ataNote')}
            </p>
          )}

          {(smartData.criticalWarning ?? 0) !== 0 && (
            <div className="glass-panel smart-banner crit" role="alert">
              <AlertTriangle size={20} color="var(--alert-red)" />
              <div>
                <strong>{tFormat('smart.critFlag', { code: smartData.criticalWarning!.toString(16) })}</strong>
                <p>{t('smart.critBody')}</p>
              </div>
            </div>
          )}

          {smartData.isSsd && (
            <div className="glass-panel smart-banner warn" role="status">
              <Zap size={20} color="var(--warning-yellow)" />
              <div>
                <strong>{t('smart.trimTitle')}</strong>
                <p>{t('smart.trimBody')}</p>
              </div>
            </div>
          )}

          <div className="smart-grid">

            <div className="smart-card glass-panel">
              <div className="smart-card-label">
                <Thermometer size={18} /> {t('smart.temperature')}
              </div>
              <div className="smart-metric">
                {smartData.temperatureC ?? 0}°C
              </div>
              <div className={`smart-hint ${(smartData.temperatureC ?? 0) > 50 ? 'warn' : 'ok'}`}>
                {(smartData.temperatureC ?? 0) > 50 ? t('smart.tempHigh') : t('smart.tempNormal')}
              </div>
            </div>

            <div className="smart-card glass-panel">
              <div className="smart-card-label">
                <Clock size={18} /> {t('smart.poh')}
              </div>
              <div className="smart-metric">
                {smartData.powerOnHours}
              </div>
              <div className="smart-hint">
                {tFormat('smart.hours', { n: String(Math.floor((smartData.powerOnHours ?? 0) / 24)) })}
              </div>
            </div>

            <div className="smart-card glass-panel">
              <div className="smart-card-label">
                <HardDrive size={18} /> {t('smart.reallocated')}
              </div>
              <div className={`smart-metric ${(smartData.reallocatedSectors ?? 0) > 0 ? 'warn' : ''}`}>
                {smartData.reallocatedSectors}
              </div>
              <div className={`smart-hint ${(smartData.reallocatedSectors ?? 0) > 0 ? 'warn' : 'ok'}`}>
                {(smartData.reallocatedSectors ?? 0) > 0 ? t('smart.reallocBad') : t('smart.noProblem')}
              </div>
            </div>

            <div className="smart-card glass-panel">
              <div className="smart-card-label">
                <AlertTriangle size={18} /> {t('smart.pending')}
              </div>
              <div className={`smart-metric ${(smartData.pendingSectors ?? 0) > 0 ? 'bad' : ''}`}>
                {smartData.pendingSectors}
              </div>
              <div className={`smart-hint ${(smartData.pendingSectors ?? 0) > 0 ? 'bad' : 'ok'}`}>
                {(smartData.pendingSectors ?? 0) > 0 ? t('smart.pendingBad') : t('smart.noProblem')}
              </div>
            </div>

            {(smartData.percentageUsed ?? -1) >= 0 && (
              <div className="smart-card glass-panel">
                <div className="smart-card-label">
                  <Zap size={18} /> {t('smart.endurance')}
                </div>
                <div className={`smart-metric ${(smartData.percentageUsed ?? 0) > 90 ? 'warn' : ''}`}>
                  {tFormat('common.percent', { n: String(smartData.percentageUsed) })}
                </div>
                <div className="smart-hint">
                  {tFormat('smart.spare', { v: smartData.availableSpare !== undefined && smartData.availableSpare >= 0 ? tFormat('common.percent', { n: String(smartData.availableSpare) }) : '—' })}
                </div>
              </div>
            )}

            {(smartData.totalBytesWritten ?? 0) > 0 && (
              <div className="smart-card glass-panel">
                <div className="smart-card-label">
                  <Database size={18} /> {t('smart.tbw')}
                </div>
                <div className="smart-metric">
                  {formatBytes(smartData.totalBytesWritten!)}
                </div>
                <div className="smart-hint">
                  {t('smart.tbwHint')}
                </div>
              </div>
            )}

            {(smartData.unsafeShutdowns ?? 0) > 0 && (
              <div className="smart-card glass-panel">
                <div className="smart-card-label">
                  <Power size={18} /> {t('smart.shutdowns')}
                </div>
                <div className="smart-metric">
                  {smartData.unsafeShutdowns}
                </div>
                <div className="smart-hint">
                  {t('smart.shutdownsHint')}
                </div>
              </div>
            )}

          </div>
        </div>
      ) : (
        <div className="smart-content empty glass-panel examiner-empty" role="alert">
          <div className="examiner-icon" aria-hidden="true">
            <AlertTriangle size={28} color="var(--warning-yellow)" />
          </div>
          <h3>{t('smart.readFailedTitle')}</h3>
          <p>{t('smart.readFailedBody')}</p>
          {smartData?.isSsd && (
            <p><strong>{t('smart.trimTitle')}</strong> — {t('smart.trimBody')}</p>
          )}
          {smartData && smartData.seekPenaltyKnown !== true && (
            <p>{t('drive.trimUnread')}</p>
          )}
        </div>
      )}
    </div>
  )
}

export default SmartView
