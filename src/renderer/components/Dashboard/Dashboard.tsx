import React, { useEffect, useRef, useState } from 'react'
import DriveCard from './DriveCard'
import SsdTrimModal from './SsdTrimModal'
import type { DriveInfo, LostPartitionHit, ResolvedVolume, ScanOptions, ScanState } from '../../../shared/types'
import { SCAN_PROFILES, mediaNeedsTrimAck, smartTrimSignals } from '../../../shared/scan-profiles'
import type { ScanProfile } from '../../../shared/scan-profiles'
import { isPausedScan, scanProgressPercent, scanShowsMetadataResume } from '../../../shared/scan-session'
import './Dashboard.css'
import InlineAlert from '../InlineAlert'
import ConfirmModal from '../ConfirmModal'
import { localizeNote, useI18n, tFormat, formatInt } from '../../i18n'
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
  const [sessionLoadError, setSessionLoadError] = useState<string | null>(null)
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
  const [volumeLettersError, setVolumeLettersError] = useState<string | null>(null)
  const [volumeResolveStatus, setVolumeResolveStatus] = useState<string | null>(null)
  const [volumeTrimOpen, setVolumeTrimOpen] = useState(false)
  const [pendingVolumeScan, setPendingVolumeScan] = useState<{
    resolved: ResolvedVolume
    scanType: ScanProfile
    unread: boolean
  } | null>(null)
  // App.tsx owns the global dbError banner — a second fetch here showed the
  // same failure twice on the dashboard.
  const [confirmClearOpen, setConfirmClearOpen] = useState(false)
  const [clearError, setClearError] = useState<string | null>(null)
  // Stale-response guard: navigation can unmount the dashboard while the
  // drive list / session probe is in flight; late .then state writes stop.
  // Setup re-arms the flag so StrictMode's double mount stays live.
  const aliveRef = useRef(true)
  useEffect(() => {
    aliveRef.current = true
    return () => { aliveRef.current = false }
  }, [])
  // P0-2: lost partition search state.
  const [lostScanDrive, setLostScanDrive] = useState(0)
  const [lostScanning, setLostScanning] = useState(false)
  const [lostStatus, setLostStatus] = useState<string | null>(null)
  const [lostPartitions, setLostPartitions] = useState<LostPartitionHit[] | null>(null)
  const [lostUnread, setLostUnread] = useState(false)

  useEffect(() => {
    fetchDrives()
    checkActiveSession()
    if (window.api?.listVolumeLetters) {
      window.api.listVolumeLetters().then((letters) => {
        if (!aliveRef.current) return
        setVolumeLettersError(null)
        if (letters?.length) {
          setVolumeLetters(letters)
          if (!letters.includes(volumeLetter)) setVolumeLetter(letters[0])
        }
      }).catch((e: unknown) => {
        console.warn('[Dashboard] listVolumeLetters failed', e)
        if (!aliveRef.current) return
        setVolumeLettersError(t('dash.volumeLettersFailed'))
      })
    }
    // eslint-disable-next-line react-hooks/exhaustive-deps
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
        if (!aliveRef.current) return
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
      if (!aliveRef.current) return
      setError(tFormat('dash.driveListError', { err: err?.message || String(err) }))
      setDrives([])
    } finally {
      if (aliveRef.current) setLoading(false)
    }
  }

  const checkActiveSession = async () => {
    if (!window.api?.getLatestUsableScanId || !window.api.getScanState) return
    try {
      const usableId = await window.api.getLatestUsableScanId()
      if (!aliveRef.current) return
      if (usableId <= 0) {
        setLatestScan(null)
        setPausedSession(null)
        setSessionLoadError(null)
        return
      }
      const state = await window.api.getScanState(usableId)
      if (!aliveRef.current) return
      if (!state || state.id <= 0) {
        setLatestScan(null)
        setPausedSession(null)
        setSessionLoadError(null)
        return
      }
      setLatestScan(state)
      setPausedSession(isPausedScan(state) ? state : null)
      setSessionLoadError(null)
    } catch {
      if (aliveRef.current) setSessionLoadError(t('dash.sessionLoadFailed'))
    }
  }

  const handleClearScans = async () => {
    if (!onClearScanData || clearBusy) return
    setClearBusy(true)
    setClearError(null)
    try {
      const cleared = await onClearScanData()
      if (cleared) {
        setPausedSession(null)
        setLatestScan(null)
      } else {
        setClearError(t('dash.clearFailed'))
      }
    } finally {
      setClearBusy(false)
    }
  }

  const driveTypeOf = (driveIndex: number): string | undefined =>
    drives.find((x) => x.index === driveIndex)?.type

  const probeTrimSignals = async (driveIndex: number): Promise<{ unread: boolean; saysSsd: boolean }> => {
    if (driveTypeOf(driveIndex) === 'SSD') return { unread: false, saysSsd: true }
    if (!window.api?.getSmartStatus) return { unread: true, saysSsd: false }
    try {
      return smartTrimSignals(await window.api.getSmartStatus(driveIndex))
    } catch {
      return { unread: true, saysSsd: false }
    }
  }

  // P0-2: the lost-partition input is free numeric — only enable the scan for
  // an index that exists in the drive list (NaN/negative/out-of-range disable).
  const lostDriveValid = drives.some((d) => d.index === lostScanDrive)

  const startVolumeScan = (resolved: ResolvedVolume, scanType: ScanProfile, extra?: ScanOptions) => {
    if (!onStartScan) return
    const base = tFormat('dash.volumeResolved', { drive: String(resolved.driveIndex), sector: String(resolved.startSector), fs: resolved.fsType })
    const spanned = (resolved.diskExtentCount ?? 1) > 1 ? `\n${t('dash.volumeSpanned')}` : ''
    setVolumeResolveStatus(base + spanned)
    const multi = (resolved.diskExtentCount ?? 1) > 1 && !!resolved.volumePath
    const disks = (resolved.diskNumbers ?? []).filter((n) => Number.isInteger(n) && n >= 0)
    onStartScan(resolved.driveIndex, scanType, {
      ...(multi
        ? { volumePath: resolved.volumePath }
        : { partitionStartSector: resolved.startSector, partitionSizeInSectors: resolved.sizeSectors }),
      ...(disks.length > 0 ? { evidenceDiskIndices: disks } : {}),
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
    const type = driveTypeOf(resolved.driveIndex)
    const sig = await probeTrimSignals(resolved.driveIndex)
    if (mediaNeedsTrimAck(scanType, type, sig)) {
      setPendingVolumeScan({
        resolved,
        scanType,
        unread: sig.unread && !sig.saysSsd && type !== 'SSD',
      })
      setVolumeTrimOpen(true)
      return
    }
    startVolumeScan(resolved, scanType)
  }

  return (
    <div className="dashboard">
      <SsdTrimModal
        open={volumeTrimOpen}
        scanType={pendingVolumeScan?.scanType ?? 'deep'}
        unknownMedia={pendingVolumeScan?.unread === true}
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
      <ConfirmModal
        open={confirmClearOpen}
        title={t('dash.clearConfirmTitle')}
        body={t('dash.clearConfirm')}
        confirmLabel={t('dash.clearConfirmYes')}
        onConfirm={() => { setConfirmClearOpen(false); void handleClearScans() }}
        onCancel={() => setConfirmClearOpen(false)}
      />
      {isAdmin === false && (
        <div className="admin-banner glass-panel">
          <ShieldAlert size={24} color="var(--alert-red)" aria-hidden="true" />
          <div className="admin-banner-text">
            <strong>{t('dash.adminBannerTitle')}</strong> {t('dash.adminBannerLead')} <em>{t('dash.adminBannerRunAs')}</em>{t('dash.adminBannerTail')}
          </div>
        </div>
      )}

      {clearError && (
        <InlineAlert variant="error" onDismiss={() => setClearError(null)}>
          {clearError}
        </InlineAlert>
      )}
      {sessionLoadError && (
        <InlineAlert variant="error" testId="dash-session-load-error">
          {sessionLoadError}
        </InlineAlert>
      )}
      {volumeLettersError && (
        <InlineAlert variant="warning" testId="dash-volume-letters-error">
          {volumeLettersError}
        </InlineAlert>
      )}

      <div className="glass-panel dash-note" role="note">
        {t('dash.evidenceNote')}
      </div>

      <div className="glass-panel scan-profile-legend" data-testid="scan-profile-legend">
        <div className="legend-title">{t('dash.profilesTitle')}</div>
        <ul>
          {(Object.keys(SCAN_PROFILES) as ScanProfile[]).map((key) => (
            <li key={key}><strong>{t(`profile.${key}.label`)}:</strong> {t(`profile.${key}.detail`)}</li>
          ))}
        </ul>
      </div>

      {/* P0-UX: advanced tools collapsed by default — the home screen shows
          the primary scan flow first; expert panels stay one click away. */}
      <details className="glass-panel dash-advanced">
        <summary>
          {t('dash.advancedTools')}
        </summary>
        <div className="dash-advanced-body">
        <div className="dash-field-row">
          <label htmlFor="fvek-hex">{t('dash.fvekLabel')}</label>
          <input
            id="fvek-hex"
            className="dash-input mono"
            aria-label={t('dash.fvekAria')}
            aria-describedby="fvek-status"
            type={fvekShow ? 'text' : 'password'}
            autoComplete="off"
            value={fvekHex}
            onChange={(e) => { setFvekHex(e.target.value); setFvekStatus(null) }}
            placeholder={t('dash.fvekPlaceholder')}
            spellCheck={false}
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
        {fvekStatus && <span id="fvek-status" className="dash-status">{fvekStatus}</span>}
        <div className="dash-field-row split">
          <label htmlFor="bitlocker-drive">{t('dash.bitlockerVolume')}</label>
          <select
            id="bitlocker-drive"
            className="dash-select"
            aria-label={t('dash.bitlockerDriveAria')}
            value={recoveryDrive}
            onChange={(e) => setRecoveryDrive(Number(e.target.value))}
          >
            {drives.map((d) => (
              <option key={d.index} value={d.index}>{d.index}: {d.model || t('dash.diskFallback')}</option>
            ))}
          </select>
        </div>
        <div className="dash-field-row">
          <label htmlFor="user-password">{t('dash.userPasswordLabel')}</label>
          <input
            id="user-password"
            className="dash-input"
            aria-label={t('dash.userPasswordAria')}
            aria-describedby="user-password-status"
            type="password"
            autoComplete="off"
            value={userPassword}
            onChange={(e) => { setUserPassword(e.target.value); setUserPasswordStatus(null) }}
            placeholder={t('dash.userPasswordPlaceholder')}
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
        {userPasswordStatus && <span id="user-password-status" className="dash-status">{userPasswordStatus}</span>}
        <div className="dash-field-row split">
          <label htmlFor="recovery-password">{t('dash.recoveryLabel')}</label>
          <input
            id="recovery-password"
            className="dash-input"
            aria-label={t('dash.recoveryAria')}
            aria-describedby="recovery-status"
            type="password"
            autoComplete="off"
            value={recoveryPassword}
            onChange={(e) => { setRecoveryPassword(e.target.value); setRecoveryStatus(null) }}
            placeholder={t('dash.recoveryPlaceholder')}
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
        {recoveryStatus && <span id="recovery-status" className="dash-status">{recoveryStatus}</span>}
      </div>

      <div className="glass-panel dash-panel">
        <div className="dash-panel-title">{t('dash.volumeScanTitle')}</div>
        <p>
          {t('dash.volumeScanHint')}
        </p>
        <div className="dash-field-row">
          <select
            className="dash-select"
            aria-label={t('dash.volumeLetterLabel')}
            value={volumeLetter}
            onChange={(e) => { setVolumeLetter(e.target.value); setVolumeResolveStatus(null) }}
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
            className="btn-secondary btn-warn-edge"
            disabled={!isAdmin || scanBusy}
            data-testid="volume-scan-full-carve"
            title={t('profile.full_carve.detail')}
            onClick={() => void requestVolumeScan('full_carve')}
          >
            <AlertTriangle size={14} /> {t('profile.full_carve.label')}
          </button>
        </div>
        {volumeResolveStatus && <span className="dash-status">{volumeResolveStatus}</span>}
      </div>

      <div className="glass-panel dash-panel">
        <div className="dash-panel-title">{t('dash.lostPartitionsTitle')}</div>
        <p>
          {t('dash.lostPartitionsHint')}
        </p>
        <div className="dash-field-row">
          <input
            type="number"
            className="dash-input narrow"
            min="0"
            value={lostScanDrive}
            onChange={(e) => setLostScanDrive(Number(e.target.value))}
            aria-label={t('dash.lostDriveAria')}
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
              setLostUnread(false)
              try {
                const found = await window.api.scanLostPartitions(lostScanDrive)
                setLostPartitions(found.partitions)
                setLostUnread(found.unread)
                setLostStatus(
                  found.partitions.length
                    ? tFormat('dash.lostFound', { n: String(found.partitions.length) })
                    : found.unread
                      ? t('dash.lostUnreadShort')
                      : t('dash.lostNone'),
                )
              } catch (e) {
                setLostStatus(e instanceof Error ? e.message : String(e))
              } finally {
                setLostScanning(false)
              }
            }}
          >
            {lostScanning ? t('dash.lostScanning') : t('dash.lostScanBtn')}
          </button>
          {lostStatus && <span className="dash-status" role="status">{lostStatus}</span>}
        </div>
        {lostUnread && (
          <InlineAlert variant="warning" testId="lost-scan-unread">{t('dash.lostUnread')}</InlineAlert>
        )}
        {lostPartitions && lostPartitions.length === 0 && (
          <div className="examiner-empty lost-empty" role="status" data-testid="lost-partitions-empty">
            <p>{lostUnread ? t('dash.lostUnreadEmpty') : t('dash.lostNone')}</p>
          </div>
        )}
        {lostPartitions && lostPartitions.length > 0 && (
          <table className="lost-table" data-testid="lost-partitions-table">
            <thead>
              <tr>
                <th>{t('dash.lostStartTh')}</th>
                <th>{t('dash.lostSizeTh')}</th>
                <th>{t('dash.lostFsTh')}</th>
              </tr>
            </thead>
            <tbody>
              {lostPartitions.map((p) => (
                <tr key={p.startSector}>
                  <td className="mono">{formatInt(p.startSector)}</td>
                  <td className="muted">{formatSize(p.sizeSectors * 512)}</td>
                  <td>{p.fs}</td>
                </tr>
              ))}
            </tbody>
          </table>
        )}
        </div>
      </details>

      {pausedSession && (
        <div className="resume-banner warn glass-panel" data-testid="paused-scan-banner">
          <div className="resume-copy">
            <Activity size={24} color="var(--warning-yellow)" aria-hidden="true" />
            <div>
              <h3>{t('dash.pausedTitle')}</h3>
              <p>
                {tFormat('dash.pausedMeta', { drive: String(pausedSession.driveIndex), type: pausedSession.scanType, pct: String(scanProgressPercent(pausedSession)), scanned: String(pausedSession.scannedSectors), total: String(pausedSession.totalSectors) })}
                {!scanShowsMetadataResume(pausedSession) ? '' : ` · ${t('dash.metadataResume')}`}
                {pausedSession.metadataComplete && (pausedSession.carveResumeSector ?? 0) > 0
                  ? ` · ${tFormat('dash.carveResume', { sector: formatInt(pausedSession.carveResumeSector!) })}`
                  : ''}
              </p>
            </div>
          </div>
          <div className="resume-actions">
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
                void (async () => {
                  if (!onStartScan) return
                  const extra: ScanOptions = { resumeScanId: pausedSession.id }
                  if (pausedSession.volumePath) extra.volumePath = pausedSession.volumePath
                  if (pausedSession.evidenceDiskIndices?.length)
                    extra.evidenceDiskIndices = pausedSession.evidenceDiskIndices
                  const type = driveTypeOf(pausedSession.driveIndex)
                  const sig = await probeTrimSignals(pausedSession.driveIndex)
                  if (mediaNeedsTrimAck(pausedSession.scanType, type, sig)) {
                    extra.allowSsdDeepScan = true
                  }
                  onStartScan(pausedSession.driveIndex, pausedSession.scanType, extra)
                })()
              }}
            >
              <Play size={16} fill="currentColor" /> {t('dash.resume')}
            </button>
          </div>
        </div>
      )}

      {scanBusy && (
        <div className="resume-banner info glass-panel">
          <div className="resume-copy">
            <Activity size={24} color="var(--accent-blue)" className="spinner" aria-hidden="true" />
            <div>
              <h3>{t('dash.scanRunningTitle')}</h3>
              <p>{t('dash.scanRunningHint')}</p>
            </div>
          </div>
          <button type="button" className="btn-primary" onClick={() => onAction && onAction('scan')}>
            <Play size={16} fill="currentColor" /> {t('dash.backToScan')}
          </button>
        </div>
      )}

      <div className="dashboard-header">
        <div>
          <h2>{t('dash.drivesTitle')}</h2>
          <p>{t('dash.drivesSubtitle')}</p>
        </div>
        <div className="dashboard-header-actions">
        <button type="button" className="btn-secondary" onClick={fetchDrives} disabled={loading}>
          <RefreshCw size={16} className={loading ? 'spinner' : ''} /> {t('dash.refresh')}
        </button>
        {onClearScanData && (
          <button
            type="button"
            className="btn-secondary"
            data-testid="clear-scan-data-btn"
            disabled={clearBusy || scanBusy}
            onClick={() => setConfirmClearOpen(true)}
          >
            <RotateCw size={16} /> {t('dash.clearScans')}
          </button>
        )}
        </div>
      </div>

      {loading ? (
        <div className="loading-state glass-panel examiner-empty" role="status">
          <RefreshCw size={32} className="spinner" />
          <p>{t('dash.probing')}</p>
        </div>
      ) : drives.length === 0 ? (
        <div className="empty-state glass-panel examiner-empty" role="status">
          <div className="examiner-icon neutral" aria-hidden="true">
            <HardDrive size={28} color="var(--text-main)" />
          </div>
          <h3>{t('dash.noDriveTitle')}</h3>
          <p>
            {error || t('dash.noDriveBody')}
          </p>
          <button type="button" className="btn-secondary" onClick={fetchDrives}>
            <RotateCw size={16} /> {t('dash.retry')}
          </button>
        </div>
      ) : (
        <div className="drive-grid">
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

      <div className="dashboard-stats">
        <div className="stat-card glass-panel">
          <div className="examiner-icon neutral"><HardDrive size={28} color="var(--text-main)" /></div>
          <div>
            <div className="stat-value">{drives.length}</div>
            <div className="stat-label">{t('dash.statDisks')}</div>
          </div>
        </div>
        <div className="stat-card glass-panel">
          <div className="examiner-icon"><Activity size={28} color="var(--accent-blue)" /></div>
          <div>
            <div className="stat-value">{scanBusy ? '1' : pausedSession ? '1' : '0'}</div>
            <div className="stat-label">{t('dash.statActive')}</div>
          </div>
        </div>
        <div className="stat-card glass-panel">
          <div className="examiner-icon ok"><FolderCheck size={28} color="var(--success-green)" /></div>
          <div>
            <div className="stat-value">{latestScan?.recoveredFiles ?? 0}</div>
            <div className="stat-label">{t('dash.statRecovered')}</div>
          </div>
        </div>
      </div>
    </div>
  )
}

export default Dashboard
