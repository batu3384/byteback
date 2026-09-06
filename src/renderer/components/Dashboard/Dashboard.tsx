import React, { useEffect, useState } from 'react'
import DriveCard from './DriveCard'
import SsdTrimModal from './SsdTrimModal'
import type { DriveInfo, ResolvedVolume, ScanOptions, ScanState } from '../../../shared/types'
import { SCAN_PROFILES, scanNeedsSsdDeepAck } from '../../../shared/scan-profiles'
import type { ScanProfile } from '../../../shared/scan-profiles'
import { isPausedScan, scanProgressPercent, scanShowsMetadataResume } from '../../../shared/scan-session'
import './Dashboard.css'
import InlineAlert from '../InlineAlert'
import { localizeNote, useI18n, tFormat } from '../../i18n'
import { formatSize } from '../ResultsView/results-view-utils'
import { ShieldAlert, RotateCw, HardDrive, RefreshCw, Activity, FolderCheck, Play, Search, AlertTriangle } from 'lucide-react'

interface DashboardProps {
  onStartScan?: (driveIndex: number, scanType: string, scanOptions?: import('../../../shared/ipc-contract').ScanOptions) => void
  onAction?: (page: any, data?: any) => void
  onOpenPausedResults?: (state: ScanState) => void
  onClearScanData?: () => Promise<boolean>
  scanBusy?: boolean
}

function Dashboard({ onStartScan, onAction, onOpenPausedResults, onClearScanData, scanBusy }: DashboardProps): React.ReactElement {
  const { t } = useI18n()
  const [drives, setDrives] = useState<DriveInfo[]>([])
  const [loading, setLoading] = useState(true)
  const [isAdmin, setIsAdmin] = useState<boolean | null>(null)
  const [error, setError] = useState<string | null>(null)
  const [pausedSession, setPausedSession] = useState<ScanState | null>(null)
  const [latestScan, setLatestScan] = useState<ScanState | null>(null)
  const [clearBusy, setClearBusy] = useState(false)
  const [fvekHex, setFvekHex] = useState('')
  const [fvekStatus, setFvekStatus] = useState<string | null>(null)
  const [fvekShow, setFvekShow] = useState(false)
  const [recoveryPassword, setRecoveryPassword] = useState('')
  const [recoveryDrive, setRecoveryDrive] = useState(0)
  const [recoveryStatus, setRecoveryStatus] = useState<string | null>(null)
  const [userPassword, setUserPassword] = useState('')
  const [userPasswordStatus, setUserPasswordStatus] = useState<string | null>(null)
  const [volumeLetters, setVolumeLetters] = useState<string[]>([])
  const [volumeLetter, setVolumeLetter] = useState('C:')
  const [volumeResolveStatus, setVolumeResolveStatus] = useState<string | null>(null)
  const [volumeTrimOpen, setVolumeTrimOpen] = useState(false)
  const [pendingVolumeScan, setPendingVolumeScan] = useState<{
    resolved: ResolvedVolume
    scanType: ScanProfile
  } | null>(null)
  const [dbError, setDbError] = useState<string | null>(null)
  // P0-2: lost partition search state.
  const [lostScanDrive, setLostScanDrive] = useState(0)
  const [lostScanning, setLostScanning] = useState(false)
  const [lostStatus, setLostStatus] = useState<string | null>(null)
  const [lostPartitions, setLostPartitions] = useState<Array<{ startSector: number; sizeSectors: number; fs: string }> | null>(null)

  useEffect(() => {
    window.api?.getDbStatus?.()
      .then((s) => {
        if (!s.ready) setDbError(s.error ?? t('dash.dbInitError'))
      })
      .catch(() => setDbError(t('dash.dbStatusError')))
  }, [])

  const fetchDrives = async () => {
    setLoading(true)
    setError(null)
    try {
      if (window.api) {
        const [adminStatus, driveList] = await Promise.all([
          window.api.isAdmin(),
          window.api.listDrives()
        ])
        setIsAdmin(adminStatus)
        setDrives(driveList as DriveInfo[])
        if (!driveList || driveList.length === 0) {
          setError(t('dash.noDrives'))
        }
      } else {
        setError(t('dash.noApi'))
      }
    } catch (err: any) {
      console.error('Sürücüler alınırken hata:', err)
      setError(tFormat('dash.driveListError', { err: err?.message || String(err) }))
      setDrives([])
    } finally {
      setLoading(false)
    }
  }

  const checkActiveSession = async () => {
    if (!window.api?.getLatestUsableScanId || !window.api.getScanState) return
    try {
      const usableId = await window.api.getLatestUsableScanId()
      if (usableId <= 0) {
        setLatestScan(null)
        setPausedSession(null)
        return
      }
      const state = await window.api.getScanState(usableId)
      if (!state || state.id <= 0) {
        setLatestScan(null)
        setPausedSession(null)
        return
      }
      setLatestScan(state)
      setPausedSession(isPausedScan(state) ? state : null)
    } catch {
      setPausedSession(null)
    }
  }

  const handleClearScans = async () => {
    if (!onClearScanData || clearBusy) return
    const ok = window.confirm(
      t('dash.clearConfirm'),
    )
    if (!ok) return
    setClearBusy(true)
    try {
      const cleared = await onClearScanData()
      if (cleared) {
        setPausedSession(null)
        setLatestScan(null)
      } else {
        window.alert(t('dash.clearFailed'))
      }
    } finally {
      setClearBusy(false)
    }
  }

  useEffect(() => {
    fetchDrives()
    checkActiveSession()
    if (window.api?.listVolumeLetters) {
      window.api.listVolumeLetters().then((letters) => {
        if (letters?.length) {
          setVolumeLetters(letters)
          if (!letters.includes(volumeLetter)) setVolumeLetter(letters[0])
        }
      }).catch((e: unknown) => console.warn('[Dashboard] listVolumeLetters failed', e))
    }
  }, [])

  const driveIsSsd = (driveIndex: number): boolean => {
    const d = drives.find((x) => x.index === driveIndex)
    return d?.type === 'SSD'
  }

  // P0-2: the lost-partition input is free numeric — only enable the scan for
  // an index that exists in the drive list (NaN/negative/out-of-range disable).
  const lostDriveValid = drives.some((d) => d.index === lostScanDrive)

  const startVolumeScan = (resolved: ResolvedVolume, scanType: ScanProfile, extra?: ScanOptions) => {
    if (!onStartScan) return
    setVolumeResolveStatus(
      tFormat('dash.volumeResolved', { drive: String(resolved.driveIndex), sector: String(resolved.startSector), fs: resolved.fsType })
    )
    onStartScan(resolved.driveIndex, scanType, {
      partitionStartSector: resolved.startSector,
      partitionSizeInSectors: resolved.sizeSectors,
      ...extra,
    })
  }

  const requestVolumeScan = async (scanType: ScanProfile) => {
    if (!isAdmin) {
      setVolumeResolveStatus(t('dash.adminRequired'))
      return
    }
    if (!window.api?.resolveVolume || !onStartScan) {
      setVolumeResolveStatus(t('dash.noApiShort'))
      return
    }
    const resolved = await window.api.resolveVolume(volumeLetter) as ResolvedVolume | null
    if (!resolved) {
      setVolumeResolveStatus(tFormat('dash.volumeResolveFailed', { letter: volumeLetter }))
      return
    }
    const needsTrim = scanNeedsSsdDeepAck(scanType)
    if (needsTrim && driveIsSsd(resolved.driveIndex)) {
      setPendingVolumeScan({ resolved, scanType })
      setVolumeTrimOpen(true)
      return
    }
    startVolumeScan(resolved, scanType, needsTrim ? { allowSsdDeepScan: true } : undefined)
  }

  return (
    <div className="dashboard" style={{ display: 'flex', flexDirection: 'column', gap: 'var(--space-lg)' }}>
      <SsdTrimModal
        open={volumeTrimOpen}
        scanType={pendingVolumeScan?.scanType ?? 'deep'}
        onConfirm={() => {
          setVolumeTrimOpen(false)
          if (pendingVolumeScan) {
            startVolumeScan(pendingVolumeScan.resolved, pendingVolumeScan.scanType, {
              allowSsdDeepScan: true,
            })
          }
          setPendingVolumeScan(null)
        }}
        onCancel={() => {
          setVolumeTrimOpen(false)
          setPendingVolumeScan(null)
        }}
      />
      {isAdmin === false && (
        <div className="admin-banner glass-panel" style={{ display: 'flex', alignItems: 'center', gap: '16px', padding: '16px 24px', borderLeft: '4px solid var(--alert-red)', background: 'rgba(239, 68, 68, 0.05)' }}>
          <ShieldAlert size={24} color="var(--alert-red)" />
          <div className="admin-banner-text" style={{ flex: 1, fontSize: '0.9rem' }}>
            <strong style={{ color: 'var(--alert-red)' }}>{t('dash.adminBannerTitle')}</strong> {t('dash.adminBannerLead')} <em>{t('dash.adminBannerRunAs')}</em>{t('dash.adminBannerTail')}
          </div>
        </div>
      )}

      {dbError && (
        <InlineAlert variant="error" title={t('dash.dbAlertTitle')}>
          {tFormat('dash.dbAlertBody', { err: dbError })}
        </InlineAlert>
      )}

      <div className="glass-panel" role="note" style={{ padding: '12px 24px', color: 'var(--text-muted)', fontSize: '0.85rem' }}>
        {t('dash.evidenceNote')}
      </div>

      <div className="glass-panel" data-testid="scan-profile-legend" style={{ padding: '16px 24px' }}>
        <div style={{ fontSize: '0.9rem', fontWeight: 600, marginBottom: '10px' }}>{t('dash.profilesTitle')}</div>
        <ul style={{ margin: 0, paddingLeft: '20px', color: 'var(--text-muted)', fontSize: '0.85rem', lineHeight: 1.6 }}>
          {(Object.keys(SCAN_PROFILES) as ScanProfile[]).map((key) => (
            <li key={key}><strong>{t(`profile.${key}.label`)}:</strong> {t(`profile.${key}.detail`)}</li>
          ))}
        </ul>
      </div>

      <div className="glass-panel" style={{ padding: '16px 24px', display: 'flex', flexDirection: 'column', gap: '12px' }}>
        <div style={{ display: 'flex', flexWrap: 'wrap', gap: '12px', alignItems: 'center' }}>
          <label htmlFor="fvek-hex" style={{ fontSize: '0.85rem', color: 'var(--text-muted)' }}>{t('dash.fvekLabel')}</label>
          <input
            id="fvek-hex"
            aria-label={t('dash.fvekAria')}
            aria-describedby="fvek-status"
            type={fvekShow ? 'text' : 'password'}
            autoComplete="off"
            value={fvekHex}
            onChange={(e) => { setFvekHex(e.target.value); setFvekStatus(null) }}
            placeholder={t('dash.fvekPlaceholder')}
            spellCheck={false}
            style={{ flex: 1, minWidth: '220px', padding: '8px', background: 'var(--bg-main)', color: 'var(--text-main)', border: '1px solid var(--panel-border)', fontFamily: 'monospace' }}
          />
          <button type="button" className="btn-secondary" onClick={() => setFvekShow((v) => !v)}>
            {fvekShow ? t('dash.hide') : t('dash.show')}
          </button>
          <button
            type="button"
            className="btn-secondary"
            onClick={async () => {
              if (!window.api?.setBitLockerFvek) {
                setFvekStatus(t('dash.noApiShort'))
                return
              }
              const hex = fvekHex.replace(/\s/g, '')
              const ok = await window.api.setBitLockerFvek(hex)
              setFvekStatus(ok ? (hex ? t('dash.fvekApplied') : t('dash.fvekCleared')) : t('dash.fvekInvalid'))
            }}
          >
            {t('dash.fvekApply')}
          </button>
        </div>
        {fvekStatus && <span id="fvek-status" style={{ fontSize: '0.85rem' }}>{fvekStatus}</span>}
        <div style={{ display: 'flex', flexWrap: 'wrap', gap: '12px', alignItems: 'center', borderTop: '1px solid var(--panel-border)', paddingTop: '12px' }}>
          <label htmlFor="bitlocker-drive" style={{ fontSize: '0.85rem', color: 'var(--text-muted)' }}>{t('dash.bitlockerVolume')}</label>
          <select
            id="bitlocker-drive"
            aria-label={t('dash.bitlockerDriveAria')}
            value={recoveryDrive}
            onChange={(e) => setRecoveryDrive(Number(e.target.value))}
            style={{ padding: '8px', background: 'var(--bg-main)', color: 'var(--text-main)', border: '1px solid var(--panel-border)' }}
          >
            {drives.map((d) => (
              <option key={d.index} value={d.index}>{d.index}: {d.model || t('dash.diskFallback')}</option>
            ))}
          </select>
        </div>
        <div style={{ display: 'flex', flexWrap: 'wrap', gap: '12px', alignItems: 'center' }}>
          <label htmlFor="user-password" style={{ fontSize: '0.85rem', color: 'var(--text-muted)' }}>{t('dash.userPasswordLabel')}</label>
          <input
            id="user-password"
            aria-label={t('dash.userPasswordAria')}
            aria-describedby="user-password-status"
            type="password"
            autoComplete="off"
            value={userPassword}
            onChange={(e) => { setUserPassword(e.target.value); setUserPasswordStatus(null) }}
            placeholder={t('dash.userPasswordPlaceholder')}
            style={{ flex: 1, minWidth: '200px', padding: '8px', background: 'var(--bg-main)', color: 'var(--text-main)', border: '1px solid var(--panel-border)' }}
          />
          <button
            type="button"
            className="btn-secondary"
            onClick={async () => {
              if (!window.api?.setBitLockerPassword) {
                setUserPasswordStatus(t('dash.noApiShort'))
                return
              }
              const err = await window.api.setBitLockerPassword(recoveryDrive, userPassword)
              setUserPasswordStatus(err ? err : t('dash.fvekEngineApplied'))
            }}
          >
            {t('dash.unlockPassword')}
          </button>
        </div>
        {userPasswordStatus && <span id="user-password-status" style={{ fontSize: '0.85rem' }}>{userPasswordStatus}</span>}
        <div style={{ display: 'flex', flexWrap: 'wrap', gap: '12px', alignItems: 'center', borderTop: '1px solid var(--panel-border)', paddingTop: '12px' }}>
          <label htmlFor="recovery-password" style={{ fontSize: '0.85rem', color: 'var(--text-muted)' }}>{t('dash.recoveryLabel')}</label>
          <input
            id="recovery-password"
            aria-label={t('dash.recoveryAria')}
            aria-describedby="recovery-status"
            type="password"
            autoComplete="off"
            value={recoveryPassword}
            onChange={(e) => { setRecoveryPassword(e.target.value); setRecoveryStatus(null) }}
            placeholder={t('dash.recoveryPlaceholder')}
            style={{ flex: 1, minWidth: '200px', padding: '8px', background: 'var(--bg-main)', color: 'var(--text-main)', border: '1px solid var(--panel-border)' }}
          />
          <button
            type="button"
            className="btn-secondary"
            onClick={async () => {
              if (!window.api?.setBitLockerRecoveryPassword) {
                setRecoveryStatus(t('dash.noApiShort'))
                return
              }
              const err = await window.api.setBitLockerRecoveryPassword(recoveryDrive, recoveryPassword)
              setRecoveryStatus(err ? localizeNote(err) : t('dash.fvekEngineApplied'))
            }}
          >
            {t('dash.unlockRecovery')}
          </button>
        </div>
        {recoveryStatus && <span id="recovery-status" style={{ fontSize: '0.85rem' }}>{recoveryStatus}</span>}
      </div>

      <div className="glass-panel" style={{ padding: '16px 24px', display: 'flex', flexDirection: 'column', gap: '12px' }}>
        <div style={{ fontSize: '0.9rem', fontWeight: 600 }}>{t('dash.volumeScanTitle')}</div>
        <p style={{ fontSize: '0.85rem', color: 'var(--text-muted)', margin: 0 }}>
          {t('dash.volumeScanHint')}
        </p>
        <div style={{ display: 'flex', flexWrap: 'wrap', gap: '12px', alignItems: 'center' }}>
          <select
            aria-label={t('dash.volumeLetterLabel')}
            value={volumeLetter}
            onChange={(e) => { setVolumeLetter(e.target.value); setVolumeResolveStatus(null) }}
            style={{ padding: '8px', background: 'var(--bg-main)', color: 'var(--text-main)', border: '1px solid var(--panel-border)' }}
          >
            {(volumeLetters.length ? volumeLetters : ['C:', 'D:']).map((l) => (
              <option key={l} value={l}>{l}</option>
            ))}
          </select>
          <button
            type="button"
            className="btn-secondary"
            disabled={!isAdmin || scanBusy}
            data-testid="volume-scan-quick"
            title={t('profile.quick.detail')}
            onClick={() => void requestVolumeScan('quick')}
          >
            {t('profile.quick.label')}
          </button>
          <button
            type="button"
            className="btn-primary"
            disabled={!isAdmin || scanBusy}
            data-testid="volume-scan-deep"
            title={t('profile.deep.detail')}
            onClick={() => void requestVolumeScan('deep')}
          >
            <Search size={16} /> {t('profile.deep.label')}
          </button>
          <button
            type="button"
            className="btn-secondary"
            disabled={!isAdmin || scanBusy}
            data-testid="volume-scan-carve-only"
            title={t('profile.carve_only.detail')}
            onClick={() => void requestVolumeScan('carve_only')}
          >
            {t('profile.carve_only.label')}
          </button>
          <button
            type="button"
            className="btn-secondary"
            disabled={!isAdmin || scanBusy}
            data-testid="volume-scan-full-carve"
            title={t('profile.full_carve.detail')}
            onClick={() => void requestVolumeScan('full_carve')}
            style={{ borderColor: 'var(--warning-yellow)' }}
          >
            <AlertTriangle size={14} /> {t('profile.full_carve.label')}
          </button>
        </div>
        {volumeResolveStatus && <span style={{ fontSize: '0.85rem', color: 'var(--text-muted)' }}>{volumeResolveStatus}</span>}
      </div>

      {/* P0-2: TestDisk-style lost partition search over the whole disk. */}
      <div className="glass-panel" style={{ padding: '16px 24px', display: 'flex', flexDirection: 'column', gap: '12px' }}>
        <div style={{ fontSize: '0.9rem', fontWeight: 600 }}>{t('dash.lostPartitionsTitle')}</div>
        <p style={{ fontSize: '0.85rem', color: 'var(--text-muted)', margin: 0 }}>
          {t('dash.lostPartitionsHint')}
        </p>
        <div style={{ display: 'flex', flexWrap: 'wrap', gap: '12px', alignItems: 'center' }}>
          <input
            type="number"
            min="0"
            value={lostScanDrive}
            onChange={(e) => setLostScanDrive(Number(e.target.value))}
            aria-label={t('dash.lostDriveAria')}
            style={{ width: '90px', padding: '8px', background: 'var(--bg-main)', color: 'var(--text-main)', border: '1px solid var(--panel-border)' }}
          />
          <button
            type="button"
            className="btn-secondary"
            disabled={!isAdmin || lostScanning || scanBusy || !lostDriveValid}
            data-testid="lost-partitions-btn"
            onClick={async () => {
              if (!window.api?.scanLostPartitions) {
                setLostStatus(t('dash.lostApiMissing'))
                return
              }
              setLostScanning(true)
              setLostStatus(t('dash.lostScanning'))
              setLostPartitions(null)
              try {
                const found = await window.api.scanLostPartitions(lostScanDrive)
                setLostPartitions(found)
                setLostStatus(found.length ? tFormat('dash.lostFound', { n: String(found.length) }) : t('dash.lostNone'))
              } catch (e) {
                setLostStatus(e instanceof Error ? e.message : String(e))
              } finally {
                setLostScanning(false)
              }
            }}
          >
            {lostScanning ? t('dash.lostScanning') : t('dash.lostScanBtn')}
          </button>
          {lostStatus && <span style={{ fontSize: '0.85rem', color: 'var(--text-muted)' }} role="status">{lostStatus}</span>}
        </div>
        {lostPartitions && lostPartitions.length > 0 && (
          <table style={{ width: '100%', borderCollapse: 'collapse', fontSize: '0.85rem' }} data-testid="lost-partitions-table">
            <thead>
              <tr>
                <th style={{ textAlign: 'left', padding: '6px 10px', borderBottom: '1px solid var(--panel-border)' }}>{t('dash.lostStartTh')}</th>
                <th style={{ textAlign: 'left', padding: '6px 10px', borderBottom: '1px solid var(--panel-border)' }}>{t('dash.lostSizeTh')}</th>
                <th style={{ textAlign: 'left', padding: '6px 10px', borderBottom: '1px solid var(--panel-border)' }}>{t('dash.lostFsTh')}</th>
              </tr>
            </thead>
            <tbody>
              {lostPartitions.map((p) => (
                <tr key={p.startSector}>
                  <td style={{ padding: '6px 10px', fontFamily: 'monospace' }}>{p.startSector.toLocaleString('tr-TR')}</td>
                  <td style={{ padding: '6px 10px', color: 'var(--text-muted)' }}>{formatSize(p.sizeSectors * 512)}</td>
                  <td style={{ padding: '6px 10px' }}>{p.fs}</td>
                </tr>
              ))}
            </tbody>
          </table>
        )}
      </div>

      {pausedSession && (
        <div className="resume-banner glass-panel" data-testid="paused-scan-banner" style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center', padding: '16px 24px', borderLeft: '4px solid var(--warning-yellow)', background: 'rgba(245, 158, 11, 0.05)' }}>
          <div style={{ display: 'flex', alignItems: 'center', gap: '16px' }}>
            <Activity size={24} color="var(--warning-yellow)" />
            <div>
              <h3 style={{ fontSize: '1rem', marginBottom: '4px' }}>{t('dash.pausedTitle')}</h3>
              <p style={{ color: 'var(--text-muted)', fontSize: '0.9rem' }}>
                {tFormat('dash.pausedMeta', { drive: String(pausedSession.driveIndex), type: pausedSession.scanType, pct: String(scanProgressPercent(pausedSession)), scanned: String(pausedSession.scannedSectors), total: String(pausedSession.totalSectors) })}
                {!scanShowsMetadataResume(pausedSession) ? '' : ` · ${t('dash.metadataResume')}`}
                {pausedSession.metadataComplete && (pausedSession.carveResumeSector ?? 0) > 0
                  ? ` · ${tFormat('dash.carveResume', { sector: pausedSession.carveResumeSector!.toLocaleString('tr-TR') })}`
                  : ''}
              </p>
            </div>
          </div>
          <div style={{ display: 'flex', gap: '8px', flexWrap: 'wrap', justifyContent: 'flex-end' }}>
            <button
              type="button"
              className="btn-secondary"
              data-testid="view-paused-results-btn"
              onClick={() => onOpenPausedResults?.(pausedSession)}
            >
              <FolderCheck size={16} /> {t('dash.viewPausedResults')}
            </button>
            <button
              type="button"
              className="btn-primary"
              data-testid="resume-scan-btn"
              disabled={scanBusy}
              onClick={() => {
                if (!onStartScan) return
                const extra: ScanOptions = { resumeScanId: pausedSession.id }
                if (scanNeedsSsdDeepAck(pausedSession.scanType) && driveIsSsd(pausedSession.driveIndex)) {
                  extra.allowSsdDeepScan = true
                }
                onStartScan(pausedSession.driveIndex, pausedSession.scanType, extra)
              }}
            >
              <Play size={16} fill="currentColor" /> {t('dash.resume')}
            </button>
          </div>
        </div>
      )}

      {scanBusy && (
        <div className="resume-banner glass-panel" style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center', padding: '16px 24px', borderLeft: '4px solid var(--accent-blue)', background: 'rgba(59, 130, 246, 0.05)' }}>
          <div style={{ display: 'flex', alignItems: 'center', gap: '16px' }}>
            <Activity size={24} color="var(--accent-blue)" className="spinner" />
            <div>
              <h3 style={{ fontSize: '1rem', marginBottom: '4px' }}>{t('dash.scanRunningTitle')}</h3>
              <p style={{ color: 'var(--text-muted)', fontSize: '0.9rem' }}>{t('dash.scanRunningHint')}</p>
            </div>
          </div>
          <button type="button" className="btn-primary" onClick={() => onAction && onAction('scan')}>
            <Play size={16} fill="currentColor" /> {t('dash.backToScan')}
          </button>
        </div>
      )}

      <div className="dashboard-header" style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'flex-end', marginTop: '16px', gap: '12px', flexWrap: 'wrap' }}>
        <div>
          <h2 style={{ fontSize: '1.5rem', marginBottom: '4px' }}>{t('dash.drivesTitle')}</h2>
          <p style={{ color: 'var(--text-muted)', fontSize: '0.95rem' }}>{t('dash.drivesSubtitle')}</p>
        </div>
        <div style={{ display: 'flex', gap: '8px', flexWrap: 'wrap' }}>
        <button className="btn-secondary" onClick={fetchDrives} disabled={loading} style={{ display: 'flex', gap: '8px' }}>
          <RefreshCw size={16} className={loading ? 'spinner' : ''} /> {t('dash.refresh')}
        </button>
        {onClearScanData && (
          <button
            type="button"
            className="btn-secondary"
            data-testid="clear-scan-data-btn"
            disabled={clearBusy || scanBusy}
            onClick={handleClearScans}
            style={{ display: 'flex', gap: '8px' }}
          >
            <RotateCw size={16} /> {t('dash.clearScans')}
          </button>
        )}
        </div>
      </div>

      {loading ? (
        <div className="loading-state glass-panel" style={{ padding: '80px', textAlign: 'center' }}>
          <RefreshCw size={32} className="spinner" style={{ margin: '0 auto 16px', color: 'var(--accent-blue)' }} />
          <p style={{ color: 'var(--text-muted)' }}>{t('dash.probing')}</p>
        </div>
      ) : drives.length === 0 ? (
        <div className="empty-state glass-panel" style={{ padding: '80px', textAlign: 'center' }}>
          <HardDrive size={48} style={{ margin: '0 auto 16px', color: 'var(--panel-border)' }} />
          <h3 style={{ marginBottom: '8px', fontSize: '1.2rem' }}>{t('dash.noDriveTitle')}</h3>
          <p style={{ color: 'var(--text-muted)', marginBottom: '24px', maxWidth: '400px', margin: '0 auto 24px', lineHeight: 1.5 }}>
            {error || t('dash.noDriveBody')}
          </p>
          <button className="btn-secondary" onClick={fetchDrives} style={{ display: 'inline-flex', gap: '8px' }}>
            <RotateCw size={16} /> {t('dash.retry')}
          </button>
        </div>
      ) : (
        <div className="drive-grid" style={{ display: 'grid', gridTemplateColumns: 'repeat(auto-fill, minmax(320px, 1fr))', gap: '24px' }}>
          {drives.map((drive) => (
            <DriveCard 
              key={drive.index} 
              drive={drive} 
              onStartScan={onStartScan} 
              onAction={onAction}
              isAdmin={isAdmin ?? false}
              diskBusy={!!scanBusy}
            />
          ))}
        </div>
      )}

      <div className="dashboard-stats" style={{ display: 'grid', gridTemplateColumns: 'repeat(3, 1fr)', gap: '24px', marginTop: '32px' }}>
        <div className="stat-card glass-panel" style={{ padding: '24px', display: 'flex', alignItems: 'center', gap: '16px' }}>
          <div style={{ background: 'var(--surface-overlay)', padding: '16px', borderRadius: '12px' }}><HardDrive size={28} color="var(--text-main)" /></div>
          <div>
            <div style={{ fontSize: '1.8rem', fontWeight: 600 }}>{drives.length}</div>
            <div style={{ fontSize: '0.9rem', color: 'var(--text-muted)' }}>{t('dash.statDisks')}</div>
          </div>
        </div>
        <div className="stat-card glass-panel" style={{ padding: '24px', display: 'flex', alignItems: 'center', gap: '16px' }}>
          <div style={{ background: 'rgba(59, 130, 246, 0.1)', padding: '16px', borderRadius: '12px' }}><Activity size={28} color="var(--accent-blue)" /></div>
          <div>
            <div style={{ fontSize: '1.8rem', fontWeight: 600 }}>{scanBusy ? '1' : pausedSession ? '1' : '0'}</div>
            <div style={{ fontSize: '0.9rem', color: 'var(--text-muted)' }}>{t('dash.statActive')}</div>
          </div>
        </div>
        <div className="stat-card glass-panel" style={{ padding: '24px', display: 'flex', alignItems: 'center', gap: '16px' }}>
          <div style={{ background: 'rgba(16, 185, 129, 0.1)', padding: '16px', borderRadius: '12px' }}><FolderCheck size={28} color="var(--success-green)" /></div>
          <div>
            <div style={{ fontSize: '1.8rem', fontWeight: 600 }}>{latestScan?.recoveredFiles ?? 0}</div>
            <div style={{ fontSize: '0.9rem', color: 'var(--text-muted)' }}>{t('dash.statRecovered')}</div>
          </div>
        </div>
      </div>
    </div>
  )
}

export default Dashboard
