import React, { useEffect, useState } from 'react'
import DriveCard from './DriveCard'
import SsdTrimModal from './SsdTrimModal'
import type { DriveInfo, ResolvedVolume, ScanOptions, ScanState } from '../../../shared/types'
import { SCAN_PROFILES } from '../../../shared/scan-profiles'
import type { ScanProfile } from '../../../shared/scan-profiles'
import { isPausedScan, scanProgressPercent } from '../../../shared/scan-session'
import './Dashboard.css'
import InlineAlert from '../InlineAlert'
import { ShieldAlert, RotateCw, HardDrive, RefreshCw, Activity, FolderCheck, Play, Search } from 'lucide-react'

interface DashboardProps {
  onStartScan?: (driveIndex: number, scanType: string, scanOptions?: import('../../../shared/ipc-contract').ScanOptions) => void
  onAction?: (page: any, data?: any) => void
  onOpenPausedResults?: (state: ScanState) => void
  onClearScanData?: () => Promise<boolean>
  scanBusy?: boolean
}

function Dashboard({ onStartScan, onAction, onOpenPausedResults, onClearScanData, scanBusy }: DashboardProps): React.ReactElement {
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

  useEffect(() => {
    window.api?.getDbStatus?.()
      .then((s) => {
        if (!s.ready) setDbError(s.error ?? 'Veritabanı başlatılamadı')
      })
      .catch(() => setDbError('Veritabanı durumu okunamadı'))
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
          setError('Hiçbir fiziksel sürücü tespit edilemedi. Uygulama yönetici izni olmadan çalışıyorsa bazı sürücüler gizlenebilir.')
        }
      } else {
        setError('Kritik Hata: window.api bulunamadı! IPC Köprüsü yüklenemedi. Lütfen uygulamayı masaüstü modunda çalıştırın.')
      }
    } catch (err: any) {
      console.error('Sürücüler alınırken hata:', err)
      setError('Sürücü listesi alınırken hata oluştu: ' + (err?.message || String(err)))
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
      'Tüm tarama kayıtları ve bulunan dosyalar SQLite\'dan silinecek. Bu işlem geri alınamaz. Devam?',
    )
    if (!ok) return
    setClearBusy(true)
    try {
      const cleared = await onClearScanData()
      if (cleared) {
        setPausedSession(null)
        setLatestScan(null)
      } else {
        window.alert('Tarama kayıtları temizlenemedi. Aktif tarama varsa önce durdurun.')
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

  const startVolumeScan = (resolved: ResolvedVolume, scanType: ScanProfile, extra?: ScanOptions) => {
    if (!onStartScan) return
    setVolumeResolveStatus(
      `PhysicalDrive${resolved.driveIndex} @ sektör ${resolved.startSector} (${resolved.fsType})`
    )
    onStartScan(resolved.driveIndex, scanType, {
      partitionStartSector: resolved.startSector,
      partitionSizeInSectors: resolved.sizeSectors,
      ...extra,
    })
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
            <strong style={{ color: 'var(--alert-red)' }}>Yönetici İzni Yok</strong> — Sürücüler listelenebilir ancak sektör tabanlı işlemler için uygulamayı <em>Yönetici Olarak Çalıştır</em> ile yeniden başlatmanız gerekir.
          </div>
        </div>
      )}

      {dbError && (
        <InlineAlert variant="error" title="Veritabanı kullanılamıyor">
          Tarama sonuçları, kurtarma ve rapor SQLite veritabanına bağlıdır. Hata: {dbError}. Uygulamayı yeniden başlatın; sorun sürerse `%APPDATA%/byteback` yazma iznini kontrol edin.
        </InlineAlert>
      )}

      <div className="glass-panel" role="note" style={{ padding: '12px 24px', color: 'var(--text-muted)', fontSize: '0.85rem' }}>
        Kanıt diski: motor GENERIC_READ. BitLocker: FVEK hex, kullanıcı parolası (0x2000) veya kurtarma parolası (0x0800); TPM/startup-key desteklenmez. İmaj ham ciphertext yazar. PhysicalDrive imhası Yok Edici’de seri + IMHA + onay ile.
      </div>

      <div className="glass-panel" data-testid="scan-profile-legend" style={{ padding: '16px 24px' }}>
        <div style={{ fontSize: '0.9rem', fontWeight: 600, marginBottom: '10px' }}>Tarama profilleri</div>
        <ul style={{ margin: 0, paddingLeft: '20px', color: 'var(--text-muted)', fontSize: '0.85rem', lineHeight: 1.6 }}>
          {(Object.keys(SCAN_PROFILES) as ScanProfile[]).map((key) => (
            <li key={key}><strong>{SCAN_PROFILES[key].label}:</strong> {SCAN_PROFILES[key].detail}</li>
          ))}
        </ul>
      </div>

      <div className="glass-panel" style={{ padding: '16px 24px', display: 'flex', flexDirection: 'column', gap: '12px' }}>
        <div style={{ display: 'flex', flexWrap: 'wrap', gap: '12px', alignItems: 'center' }}>
          <label htmlFor="fvek-hex" style={{ fontSize: '0.85rem', color: 'var(--text-muted)' }}>BitLocker FVEK (64 veya 128 hex)</label>
          <input
            id="fvek-hex"
            aria-label="BitLocker FVEK hex"
            aria-describedby="fvek-status"
            type={fvekShow ? 'text' : 'password'}
            autoComplete="off"
            value={fvekHex}
            onChange={(e) => { setFvekHex(e.target.value); setFvekStatus(null) }}
            placeholder="boş = anahtarı temizle"
            spellCheck={false}
            style={{ flex: 1, minWidth: '220px', padding: '8px', background: 'var(--bg-main)', color: 'var(--text-main)', border: '1px solid var(--panel-border)', fontFamily: 'monospace' }}
          />
          <button type="button" className="btn-secondary" onClick={() => setFvekShow((v) => !v)}>
            {fvekShow ? 'Gizle' : 'Göster'}
          </button>
          <button
            type="button"
            className="btn-secondary"
            onClick={async () => {
              if (!window.api?.setBitLockerFvek) {
                setFvekStatus('API yok')
                return
              }
              const hex = fvekHex.replace(/\s/g, '')
              const ok = await window.api.setBitLockerFvek(hex)
              setFvekStatus(ok ? (hex ? 'FVEK tarama/kurtarma/hex okumasına uygulandı' : 'FVEK temizlendi') : 'Geçersiz (64/128 hex) veya motor hatası')
            }}
          >
            FVEK uygula
          </button>
        </div>
        {fvekStatus && <span id="fvek-status" style={{ fontSize: '0.85rem' }}>{fvekStatus}</span>}
        <div style={{ display: 'flex', flexWrap: 'wrap', gap: '12px', alignItems: 'center', borderTop: '1px solid var(--panel-border)', paddingTop: '12px' }}>
          <label htmlFor="bitlocker-drive" style={{ fontSize: '0.85rem', color: 'var(--text-muted)' }}>BitLocker birimi</label>
          <select
            id="bitlocker-drive"
            aria-label="BitLocker sürücüsü"
            value={recoveryDrive}
            onChange={(e) => setRecoveryDrive(Number(e.target.value))}
            style={{ padding: '8px', background: 'var(--bg-main)', color: 'var(--text-main)', border: '1px solid var(--panel-border)' }}
          >
            {drives.map((d) => (
              <option key={d.index} value={d.index}>{d.index}: {d.model || 'disk'}</option>
            ))}
          </select>
        </div>
        <div style={{ display: 'flex', flexWrap: 'wrap', gap: '12px', alignItems: 'center' }}>
          <label htmlFor="user-password" style={{ fontSize: '0.85rem', color: 'var(--text-muted)' }}>Kullanıcı parolası (0x2000)</label>
          <input
            id="user-password"
            aria-label="BitLocker kullanıcı parolası"
            aria-describedby="user-password-status"
            type="password"
            autoComplete="off"
            value={userPassword}
            onChange={(e) => { setUserPassword(e.target.value); setUserPasswordStatus(null) }}
            placeholder="Windows oturum parolası"
            style={{ flex: 1, minWidth: '200px', padding: '8px', background: 'var(--bg-main)', color: 'var(--text-main)', border: '1px solid var(--panel-border)' }}
          />
          <button
            type="button"
            className="btn-secondary"
            onClick={async () => {
              if (!window.api?.setBitLockerPassword) {
                setUserPasswordStatus('API yok')
                return
              }
              const err = await window.api.setBitLockerPassword(recoveryDrive, userPassword)
              setUserPasswordStatus(err ? err : 'FVEK motor okumasına uygulandı')
            }}
          >
            Paroladan aç
          </button>
        </div>
        {userPasswordStatus && <span id="user-password-status" style={{ fontSize: '0.85rem' }}>{userPasswordStatus}</span>}
        <div style={{ display: 'flex', flexWrap: 'wrap', gap: '12px', alignItems: 'center', borderTop: '1px solid var(--panel-border)', paddingTop: '12px' }}>
          <label htmlFor="recovery-password" style={{ fontSize: '0.85rem', color: 'var(--text-muted)' }}>Kurtarma parolası (0x0800)</label>
          <input
            id="recovery-password"
            aria-label="BitLocker kurtarma parolası"
            aria-describedby="recovery-status"
            type="password"
            autoComplete="off"
            value={recoveryPassword}
            onChange={(e) => { setRecoveryPassword(e.target.value); setRecoveryStatus(null) }}
            placeholder="48-digit veya recovery password"
            style={{ flex: 1, minWidth: '200px', padding: '8px', background: 'var(--bg-main)', color: 'var(--text-main)', border: '1px solid var(--panel-border)' }}
          />
          <button
            type="button"
            className="btn-secondary"
            onClick={async () => {
              if (!window.api?.setBitLockerRecoveryPassword) {
                setRecoveryStatus('API yok')
                return
              }
              const err = await window.api.setBitLockerRecoveryPassword(recoveryDrive, recoveryPassword)
              setRecoveryStatus(err ? err : 'FVEK motor okumasına uygulandı')
            }}
          >
            Kurtarmadan aç
          </button>
        </div>
        {recoveryStatus && <span id="recovery-status" style={{ fontSize: '0.85rem' }}>{recoveryStatus}</span>}
      </div>

      <div className="glass-panel" style={{ padding: '16px 24px', display: 'flex', flexDirection: 'column', gap: '12px' }}>
        <div style={{ fontSize: '0.9rem', fontWeight: 600 }}>Mantıksal sürücüden tara</div>
        <p style={{ fontSize: '0.85rem', color: 'var(--text-muted)', margin: 0 }}>
          Harf (ör. D:) → PhysicalDrive + bölüm ofseti. Yalnızca o birimi tarar.
        </p>
        <div style={{ display: 'flex', flexWrap: 'wrap', gap: '12px', alignItems: 'center' }}>
          <select
            aria-label="Mantıksal sürücü harfi"
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
            className="btn-primary"
            disabled={!isAdmin}
            onClick={async () => {
              if (!isAdmin) {
                setVolumeResolveStatus('Yönetici izni gerekli.')
                return
              }
              if (!window.api?.resolveVolume || !onStartScan) {
                setVolumeResolveStatus('API yok')
                return
              }
              const resolved = await window.api.resolveVolume(volumeLetter) as ResolvedVolume | null
              if (!resolved) {
                setVolumeResolveStatus(`${volumeLetter} çözülemedi (erişim veya harf hatalı)`)
                return
              }
              if (driveIsSsd(resolved.driveIndex)) {
                setPendingVolumeScan({ resolved, scanType: 'deep' })
                setVolumeTrimOpen(true)
                return
              }
              startVolumeScan(resolved, 'deep')
            }}
          >
            <Search size={16} /> {volumeLetter} derin tara
          </button>
        </div>
        {volumeResolveStatus && <span style={{ fontSize: '0.85rem', color: 'var(--text-muted)' }}>{volumeResolveStatus}</span>}
      </div>

      {pausedSession && (
        <div className="resume-banner glass-panel" data-testid="paused-scan-banner" style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center', padding: '16px 24px', borderLeft: '4px solid var(--warning-yellow)', background: 'rgba(245, 158, 11, 0.05)' }}>
          <div style={{ display: 'flex', alignItems: 'center', gap: '16px' }}>
            <Activity size={24} color="var(--warning-yellow)" />
            <div>
              <h3 style={{ fontSize: '1rem', marginBottom: '4px' }}>Yarım Kalan Tarama</h3>
              <p style={{ color: 'var(--text-muted)', fontSize: '0.9rem' }}>
                Sürücü {pausedSession.driveIndex} · {pausedSession.scanType} · %{scanProgressPercent(pausedSession)} ({pausedSession.scannedSectors} / {pausedSession.totalSectors} sektör)
                {!pausedSession.metadataComplete && pausedSession.scanType !== 'quick' ? ' · metadata aşamasından devam edilecek' : ''}
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
              <FolderCheck size={16} /> Sonuçları gör
            </button>
            <button
              className="btn-primary"
              data-testid="resume-scan-btn"
              onClick={() => onStartScan && onStartScan(pausedSession.driveIndex, pausedSession.scanType, { resumeScanId: pausedSession.id })}
            >
              <Play size={16} fill="currentColor" /> Devam et
            </button>
          </div>
        </div>
      )}

      {scanBusy && (
        <div className="resume-banner glass-panel" style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center', padding: '16px 24px', borderLeft: '4px solid var(--accent-blue)', background: 'rgba(59, 130, 246, 0.05)' }}>
          <div style={{ display: 'flex', alignItems: 'center', gap: '16px' }}>
            <Activity size={24} color="var(--accent-blue)" className="spinner" />
            <div>
              <h3 style={{ fontSize: '1rem', marginBottom: '4px' }}>Tarama sürüyor</h3>
              <p style={{ color: 'var(--text-muted)', fontSize: '0.9rem' }}>Tarama ekranından ilerlemeyi izleyebilirsiniz.</p>
            </div>
          </div>
          <button type="button" className="btn-primary" onClick={() => onAction && onAction('scan')}>
            <Play size={16} fill="currentColor" /> Taramaya dön
          </button>
        </div>
      )}

      <div className="dashboard-header" style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'flex-end', marginTop: '16px', gap: '12px', flexWrap: 'wrap' }}>
        <div>
          <h2 style={{ fontSize: '1.5rem', marginBottom: '4px' }}>Sistemdeki Sürücüler</h2>
          <p style={{ color: 'var(--text-muted)', fontSize: '0.95rem' }}>Erişilebilir tüm fiziksel donanımlar</p>
        </div>
        <div style={{ display: 'flex', gap: '8px', flexWrap: 'wrap' }}>
        <button className="btn-secondary" onClick={fetchDrives} disabled={loading} style={{ display: 'flex', gap: '8px' }}>
          <RefreshCw size={16} className={loading ? 'spinner' : ''} /> Yenile
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
            <RotateCw size={16} /> Tarama kayıtlarını temizle
          </button>
        )}
        </div>
      </div>

      {loading ? (
        <div className="loading-state glass-panel" style={{ padding: '80px', textAlign: 'center' }}>
          <RefreshCw size={32} className="spinner" style={{ margin: '0 auto 16px', color: 'var(--accent-blue)' }} />
          <p style={{ color: 'var(--text-muted)' }}>Sürücüler donanım seviyesinde sorgulanıyor...</p>
        </div>
      ) : drives.length === 0 ? (
        <div className="empty-state glass-panel" style={{ padding: '80px', textAlign: 'center' }}>
          <HardDrive size={48} style={{ margin: '0 auto 16px', color: 'var(--panel-border)' }} />
          <h3 style={{ marginBottom: '8px', fontSize: '1.2rem' }}>Sürücü Bulunamadı</h3>
          <p style={{ color: 'var(--text-muted)', marginBottom: '24px', maxWidth: '400px', margin: '0 auto 24px', lineHeight: 1.5 }}>
            {error || 'Sisteminizde desteklenen fiziksel bir disk tespit edilemedi. Lütfen bağlantıları kontrol edin.'}
          </p>
          <button className="btn-secondary" onClick={fetchDrives} style={{ display: 'inline-flex', gap: '8px' }}>
            <RotateCw size={16} /> Tekrar Dene
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
          <div style={{ background: 'rgba(255,255,255,0.05)', padding: '16px', borderRadius: '12px' }}><HardDrive size={28} color="var(--text-main)" /></div>
          <div>
            <div style={{ fontSize: '1.8rem', fontWeight: 600 }}>{drives.length}</div>
            <div style={{ fontSize: '0.9rem', color: 'var(--text-muted)' }}>Bulunan Disk</div>
          </div>
        </div>
        <div className="stat-card glass-panel" style={{ padding: '24px', display: 'flex', alignItems: 'center', gap: '16px' }}>
          <div style={{ background: 'rgba(59, 130, 246, 0.1)', padding: '16px', borderRadius: '12px' }}><Activity size={28} color="var(--accent-blue)" /></div>
          <div>
            <div style={{ fontSize: '1.8rem', fontWeight: 600 }}>{scanBusy ? '1' : pausedSession ? '1' : '0'}</div>
            <div style={{ fontSize: '0.9rem', color: 'var(--text-muted)' }}>Bekleyen / aktif tarama</div>
          </div>
        </div>
        <div className="stat-card glass-panel" style={{ padding: '24px', display: 'flex', alignItems: 'center', gap: '16px' }}>
          <div style={{ background: 'rgba(16, 185, 129, 0.1)', padding: '16px', borderRadius: '12px' }}><FolderCheck size={28} color="var(--success-green)" /></div>
          <div>
            <div style={{ fontSize: '1.8rem', fontWeight: 600 }}>{latestScan?.recoveredFiles ?? 0}</div>
            <div style={{ fontSize: '0.9rem', color: 'var(--text-muted)' }}>Kurtarılan (son tarama)</div>
          </div>
        </div>
      </div>
    </div>
  )
}

export default Dashboard
